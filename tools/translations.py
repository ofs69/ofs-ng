#!/usr/bin/env python3
"""Manage ofs-ng translation catalogs from a single place.

The localization pipeline has one English source of truth
(`localization/strings.toml`) and one `lang/<id>.toml` per language that mirrors
it, adding a `translation` field per key. Keeping a dozen of those files in step
with the source by hand — adding new keys, dropping removed ones, noticing when an
English string changed out from under a translation — is the maintenance cost this
tool removes. The structural bookkeeping is mechanical, so Python owns it; a human
(or Claude) only ever supplies `translation` strings.

Two locations hold language files:

  lang/                  Shipped, strictly validated at build time (`localization_gen
                         --validate`). A file here MUST be complete: every source key
                         present with a non-empty, placeholder-matching translation.
  localization/wip/      Work in progress. Not globbed by the build, not shipped, may
                         be incomplete. Seed new languages here, translate, then
                         `promote` into lang/ once complete.

Subcommands:

  status [id ...]    Per-language completion report (translated / missing / stale /
                     placeholder-mismatch). No args = every language file found.
  sync [id ...]      Rewrite language file(s) to mirror the source: add missing keys
                     (empty translation), drop keys no longer in the source, refresh
                     the English reference of untranslated keys, and preserve existing
                     translations. The one command to run after editing strings.toml.
  new <code>         Create a work-in-progress stub for a new language under wip/.
  todo <id>          Write a focused batch file holding only the keys that still need
                     work (missing / empty / stale / bad-placeholder), with English +
                     description + placeholder docs — the unit of work to translate.
  apply <id>         Merge a filled-in batch file back into the language file, validating
                     placeholders and clearing the stale flag. Implies a sync.
  check [id ...]     Validate language file(s) exactly as the build does, without a
                     build. No args = every file in lang/. Non-zero exit on any error.
  promote <id>       Move a complete, valid wip/ language into lang/ so it ships.

"Stale" means the source English changed after the key was translated: the file's
stored `english` reference no longer matches the source, so the translation is likely
out of date. `sync` preserves the old reference (rather than silently refreshing it)
so the mismatch survives as the signal; translating the key via `apply` clears it.

Run from anywhere; paths are resolved relative to the repo root.
"""

from __future__ import annotations

import argparse
import re
import sys
import tomllib
from dataclasses import dataclass, field
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SOURCE = REPO / "localization" / "strings.toml"
SHIP_DIR = REPO / "lang"
WIP_DIR = REPO / "localization" / "wip"

# Field-name column: pad names so the `=` aligns, matching the existing lang files
# ("translation" is the longest name we emit).
_FIELD_W = len("translation")

# Built-in metadata for the languages we seed, so `new <code>` fills in a sensible
# display name and ISO 639 code without flags. Anything not listed still works; pass
# --name / --iso639 explicitly. Display names carry the endonym for the picker.
KNOWN_LANGS: dict[str, tuple[str, str]] = {
    "zh": ("Simplified Chinese (简体中文)", "zh"),
    "zh-Hant": ("Traditional Chinese (繁體中文)", "zh"),
    "es": ("Spanish (Español)", "es"),
    "de": ("German (Deutsch)", "de"),
    "fr": ("French (Français)", "fr"),
    "ru": ("Russian (Русский)", "ru"),
    "pt-BR": ("Brazilian Portuguese (Português do Brasil)", "pt"),
    "ko": ("Korean (한국어)", "ko"),
    "it": ("Italian (Italiano)", "it"),
    "pl": ("Polish (Polski)", "pl"),
    "tr": ("Turkish (Türkçe)", "tr"),
    "ja": ("Japanese (日本語)", "ja"),
}

AI_SUFFIX = "_[AI]"
# Transient `todo` output lives alongside wip language files but is not one; exclude it from globs.
BATCH_SUFFIX = ".batch.toml"

_KEY_HEADER = re.compile(r"^\[([A-Za-z][A-Za-z0-9_]*)\]")
# A {N} placeholder reference: an unescaped '{' followed by digits and '}'.
_PLACEHOLDER = re.compile(r"(?<!\{)\{(\d+)\}(?!\})")


# ─────────────────────────────────────────────────────────────────────────────
# Model
# ─────────────────────────────────────────────────────────────────────────────

@dataclass
class Entry:
    """A source key: the English text plus translator-facing context."""

    english: str
    description: str | None = None
    placeholders: list[str] = field(default_factory=list)

    @property
    def ph(self) -> frozenset[int]:
        return placeholder_indices(self.english)


def placeholder_indices(text: str) -> frozenset[int]:
    """The set of {N} indices in `text`, honoring {{ and }} escapes.

    Matches the lenient mask the C++ runtime/validator compares on: order does not
    matter, only which indices are present.
    """
    return frozenset(int(m.group(1)) for m in _PLACEHOLDER.finditer(text))


def toml_string(s: str) -> str:
    """Render `s` as a TOML basic string. Non-ASCII is kept literal (valid UTF-8)."""
    out = ['"']
    for ch in s:
        if ch == "\\":
            out.append("\\\\")
        elif ch == '"':
            out.append('\\"')
        elif ch == "\n":
            out.append("\\n")
        elif ch == "\t":
            out.append("\\t")
        elif ch == "\r":
            out.append("\\r")
        elif ord(ch) < 0x20:
            out.append(f"\\u{ord(ch):04x}")
        else:
            out.append(ch)
    out.append('"')
    return "".join(out)


def _field_line(name: str, value: str) -> str:
    return f"{name.ljust(_FIELD_W)} = {toml_string(value)}"


# ─────────────────────────────────────────────────────────────────────────────
# Reading
# ─────────────────────────────────────────────────────────────────────────────

def load_source() -> dict[str, Entry]:
    """Parse strings.toml into {key: Entry} (insertion order = file order)."""
    with SOURCE.open("rb") as f:
        data = tomllib.load(f)
    entries: dict[str, Entry] = {}
    for key, tbl in data.items():
        if key.startswith("_"):
            continue
        if not isinstance(tbl, dict) or "english" not in tbl:
            die(f"{rel(SOURCE)}: key '{key}' is malformed (no english field)")
        entries[key] = Entry(
            english=tbl["english"],
            description=tbl.get("description"),
            placeholders=list(tbl.get("placeholders", [])),
        )
    return entries


def _is_separator_comment(text: str) -> bool:
    body = text.lstrip("#").strip()
    return len(body) >= 3 and any(c != " " for c in body) and all(c in "─—-= " for c in body)


def source_layout() -> list[tuple]:
    """Source items from the first section banner onward, for mirroring into lang files.

    Returns a list of ('comment', text) / ('blank',) / ('key', name) tuples. Skips the
    file's leading schema doc (everything before the first separator banner or key) so a
    generated lang file starts clean, then replays the section comments and blank lines
    verbatim so translators see the same grouping the source has.
    """
    lines = SOURCE.read_text(encoding="utf-8").split("\n")
    items: list[tuple] = []
    i, n = 0, len(lines)
    while i < n:
        stripped = lines[i].strip()
        if not stripped:
            items.append(("blank",))
            i += 1
        elif stripped.startswith("#"):
            items.append(("comment", lines[i].rstrip()))
            i += 1
        elif (m := _KEY_HEADER.match(stripped)):
            items.append(("key", m.group(1)))
            i += 1
            # Skip the key's body (english/description/placeholders, the last possibly a
            # multi-line array) up to the next blank, comment, or table header.
            while i < n:
                s2 = lines[i].strip()
                if s2 == "" or s2.startswith("#") or s2.startswith("["):
                    break
                i += 1
        else:
            i += 1  # stray line (none expected in the source)

    for idx, it in enumerate(items):
        if it[0] == "key" or (it[0] == "comment" and _is_separator_comment(it[1])):
            return items[idx:]
    return items


@dataclass
class LangFile:
    lang_id: str
    path: Path
    iso639: str
    # key -> (english_reference, translation)
    entries: dict[str, tuple[str, str]]
    shipped: bool


def load_lang(path: Path) -> LangFile:
    with path.open("rb") as f:
        data = tomllib.load(f)
    meta = data.get("_meta", {})
    iso = meta.get("iso639", "") if isinstance(meta, dict) else ""
    entries: dict[str, tuple[str, str]] = {}
    for key, tbl in data.items():
        if key.startswith("_"):
            continue
        if not isinstance(tbl, dict):
            continue
        entries[key] = (tbl.get("english", ""), tbl.get("translation", ""))
    return LangFile(
        lang_id=path.stem,
        path=path,
        iso639=iso,
        entries=entries,
        shipped=path.parent == SHIP_DIR,
    )


def find_lang_path(lang_id: str) -> Path | None:
    """Locate a language by id, preferring a shipped file over a wip one."""
    for d in (SHIP_DIR, WIP_DIR):
        p = d / f"{lang_id}.toml"
        if p.exists():
            return p
    return None


def all_lang_paths() -> list[Path]:
    paths: list[Path] = []
    for d in (SHIP_DIR, WIP_DIR):
        if d.is_dir():
            paths.extend(sorted(p for p in d.glob("*.toml") if not p.name.endswith(BATCH_SUFFIX)))
    return paths


def resolve_ids(ids: list[str]) -> list[Path]:
    if not ids:
        return all_lang_paths()
    out: list[Path] = []
    for lang_id in ids:
        p = find_lang_path(lang_id)
        if p is None:
            die(f"no language file for '{lang_id}' in lang/ or localization/wip/")
        out.append(p)
    return out


# ─────────────────────────────────────────────────────────────────────────────
# Status classification
# ─────────────────────────────────────────────────────────────────────────────

# Per-key state of a translation relative to the source.
OK, MISSING, EMPTY, STALE, BADPH = "ok", "missing", "empty", "stale", "badph"
NEEDS_WORK = (MISSING, EMPTY, STALE, BADPH)


def classify(key: str, entry: Entry, lang: LangFile) -> str:
    pair = lang.entries.get(key)
    if pair is None:
        return MISSING
    eng_ref, tr = pair
    if tr.strip() == "":
        return EMPTY
    if placeholder_indices(tr) != entry.ph:
        return BADPH
    if eng_ref != entry.english:
        return STALE
    return OK


def status_counts(source: dict[str, Entry], lang: LangFile) -> dict[str, int]:
    counts = {OK: 0, MISSING: 0, EMPTY: 0, STALE: 0, BADPH: 0}
    for key, entry in source.items():
        counts[classify(key, entry, lang)] += 1
    return counts


# ─────────────────────────────────────────────────────────────────────────────
# Rendering
# ─────────────────────────────────────────────────────────────────────────────

def build_header(lang_id: str, name: str) -> list[str]:
    is_ai = AI_SUFFIX in lang_id
    lines = [f"# {lang_id}.toml — {name} translation for ofs-ng.", "#"]
    if is_ai:
        lines += [
            "# AI-generated translation. The \"_[AI]\" filename suffix is part of the language id and is",
            "# shown verbatim in the language picker, so users can tell machine translations from",
            "# human-reviewed ones.",
            "#",
        ]
    lines += [
        "# Managed by tools/translations.py — run `python tools/translations.py sync` after the source",
        "# catalog changes. Only the `translation` field is authored by hand; `english` is a read-only",
        "# reference used to detect when a source string changed (a stale translation). For formatted",
        "# strings the set of {N} placeholders in `translation` must match the source.",
    ]
    return lines


def render_lang(lang_id: str, name: str, iso639: str, source: dict[str, Entry],
                layout: list[tuple], translations: dict[str, tuple[str, str]]) -> str:
    out: list[str] = build_header(lang_id, name)
    out += ["", "[_meta]", f'iso639 = "{iso639}"', ""]
    for item in layout:
        if item[0] == "blank":
            out.append("")
        elif item[0] == "comment":
            out.append(item[1])
        elif item[0] == "key":
            key = item[1]
            entry = source[key]
            eng_ref, tr = translations.get(key, (entry.english, ""))
            out.append(f"[{key}]")
            out.append(_field_line("english", eng_ref))
            out.append(_field_line("translation", tr))
    text = "\n".join(out).rstrip("\n") + "\n"
    return text


def sync_translations(source: dict[str, Entry],
                      existing: dict[str, tuple[str, str]]) -> tuple[dict[str, tuple[str, str]], dict[str, list[str]]]:
    """Compute the synced translation map plus a report of what changed.

    For an untranslated key the English reference is refreshed to the current source
    (it is only a reference). For a translated key the stored reference is preserved so
    a source change stays visible as a stale flag until the key is re-translated.
    """
    result: dict[str, tuple[str, str]] = {}
    report = {"added": [], "removed": [], "stale": [], "badph": []}
    for key, entry in source.items():
        pair = existing.get(key)
        if pair is None:
            result[key] = (entry.english, "")
            report["added"].append(key)
            continue
        eng_ref, tr = pair
        if tr.strip() == "":
            result[key] = (entry.english, "")
        else:
            result[key] = (eng_ref, tr)
            if placeholder_indices(tr) != entry.ph:
                report["badph"].append(key)
            elif eng_ref != entry.english:
                report["stale"].append(key)
    for key in existing:
        if key not in source:
            report["removed"].append(key)
    return result, report


# ─────────────────────────────────────────────────────────────────────────────
# Commands
# ─────────────────────────────────────────────────────────────────────────────

def cmd_status(args: argparse.Namespace) -> int:
    source = load_source()
    total = len(source)
    paths = resolve_ids(args.ids)
    if not paths:
        print("No language files found. Seed one with: translations.py new <code>")
        return 0
    name_w = max(len(p.stem) for p in paths)
    print(f"{'language'.ljust(name_w)}  {'done':>9}   missing  empty  stale  badph  loc")
    for p in paths:
        lang = load_lang(p)
        c = status_counts(source, lang)
        pct = 100.0 * c[OK] / total if total else 100.0
        loc = "ship" if lang.shipped else "wip"
        print(f"{lang.lang_id.ljust(name_w)}  {c[OK]:>4}/{total:<4} {pct:4.0f}%  "
              f"{c[MISSING]:>7}  {c[EMPTY]:>5}  {c[STALE]:>5}  {c[BADPH]:>5}  {loc}")
    return 0


def cmd_sync(args: argparse.Namespace) -> int:
    source = load_source()
    layout = source_layout()
    paths = resolve_ids(args.ids)
    for p in paths:
        lang = load_lang(p)
        name = lang_display_name(lang.lang_id)
        synced, report = sync_translations(source, lang.entries)
        text = render_lang(lang.lang_id, name, lang.iso639 or "en", source, layout, synced)
        changed = text != p.read_text(encoding="utf-8")
        if changed:
            p.write_text(text, encoding="utf-8", newline="\n")
        bits = []
        for k in ("added", "removed", "stale", "badph"):
            if report[k]:
                bits.append(f"{len(report[k])} {k}")
        if not bits:
            summary = "reformatted" if changed else "up to date"
        else:
            summary = ", ".join(bits)
        print(f"{rel(p)}: {summary}")
        if lang.shipped and report["added"]:
            print(f"  WARNING: {len(report['added'])} new key(s) are now untranslated in a SHIPPED file; "
                  f"the build will fail until they are translated. Complete them or move the file to "
                  f"localization/wip/.")
    return 0


def cmd_new(args: argparse.Namespace) -> int:
    code = args.code
    name, iso = KNOWN_LANGS.get(code, (code, code))
    if args.name:
        name = args.name
    if args.iso639:
        iso = args.iso639
    lang_id = code + (AI_SUFFIX if args.ai else "")
    if find_lang_path(lang_id) is not None:
        die(f"language '{lang_id}' already exists")
    WIP_DIR.mkdir(parents=True, exist_ok=True)
    source = load_source()
    layout = source_layout()
    translations = {key: (entry.english, "") for key, entry in source.items()}
    text = render_lang(lang_id, name, iso, source, layout, translations)
    dest = WIP_DIR / f"{lang_id}.toml"
    dest.write_text(text, encoding="utf-8", newline="\n")
    print(f"created {rel(dest)} — {len(source)} keys, 0 translated ({name}, iso639={iso})")
    print(f"  translate it, then: translations.py promote {lang_id}")
    return 0


def cmd_todo(args: argparse.Namespace) -> int:
    source = load_source()
    p = find_lang_path(args.id)
    if p is None:
        die(f"no language file for '{args.id}'")
    lang = load_lang(p)
    pending: list[tuple[str, str]] = []  # (key, state)
    for key, entry in source.items():
        st = classify(key, entry, lang)
        if st in NEEDS_WORK:
            pending.append((key, st))
    if not pending:
        print(f"{args.id}: nothing to translate — fully up to date.")
        return 0

    out: list[str] = [
        f"# Translation batch for {args.id} — {len(pending)} key(s) need work.",
        "#",
        "# Fill each `translation`. Keep every {N} placeholder from the source (any order). Leave a key",
        "# blank to skip it. When done:  python tools/translations.py apply " + args.id,
        "#",
        "# state: missing=new key  empty=untranslated  stale=source English changed  badph=placeholders",
        "#        no longer match. For a stale key, `current` shows the old translation to revise.",
        "",
    ]
    for key, st in pending:
        entry = source[key]
        out.append(f"[{key}]")
        out.append(f"# state: {st}")
        if entry.description:
            out.append(f"# context: {entry.description}")
        for doc in entry.placeholders:
            out.append(f"# placeholder: {doc}")
        out.append(_field_line("english", entry.english))
        # For a stale/bad-placeholder key the existing translation is pre-filled so it can be reviewed
        # and tweaked in place rather than redone from scratch; new/empty keys start blank.
        prefill = ""
        if st in (STALE, BADPH):
            _, prefill = lang.entries.get(key, ("", ""))
        out.append(_field_line("translation", prefill))
        out.append("")
    text = "\n".join(out).rstrip("\n") + "\n"

    dest = Path(args.out) if args.out else WIP_DIR / f"{args.id}.batch.toml"
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_text(text, encoding="utf-8", newline="\n")
    print(f"wrote {rel(dest)} — {len(pending)} key(s) to translate")
    return 0


def cmd_apply(args: argparse.Namespace) -> int:
    source = load_source()
    layout = source_layout()
    p = find_lang_path(args.id)
    if p is None:
        die(f"no language file for '{args.id}'")
    batch_path = Path(args.input) if args.input else WIP_DIR / f"{args.id}.batch.toml"
    if not batch_path.exists():
        die(f"batch file not found: {rel(batch_path)} (run `todo {args.id}` first)")
    with batch_path.open("rb") as f:
        batch = tomllib.load(f)

    lang = load_lang(p)
    merged = dict(lang.entries)
    applied, skipped, rejected = 0, 0, 0
    for key, tbl in batch.items():
        if key.startswith("_") or not isinstance(tbl, dict):
            continue
        if key not in source:
            print(f"  skip '{key}': not a source key")
            skipped += 1
            continue
        tr = tbl.get("translation", "")
        if tr.strip() == "":
            skipped += 1
            continue
        if placeholder_indices(tr) != source[key].ph:
            print(f"  reject '{key}': placeholders {sorted(placeholder_indices(tr))} != "
                  f"source {sorted(source[key].ph)}")
            rejected += 1
            continue
        # Stamp the current source English as the reference so the key is no longer stale.
        merged[key] = (source[key].english, tr)
        applied += 1

    synced, _ = sync_translations(source, merged)
    text = render_lang(lang.lang_id, lang_display_name(lang.lang_id), lang.iso639 or "en",
                       source, layout, synced)
    p.write_text(text, encoding="utf-8", newline="\n")
    print(f"{rel(p)}: applied {applied}, skipped {skipped}, rejected {rejected}")
    return 1 if rejected else 0


def validate_lang(source: dict[str, Entry], path: Path) -> list[str]:
    """Reproduce the C++ build validator's checks for one file."""
    errors: list[str] = []
    try:
        with path.open("rb") as f:
            data = tomllib.load(f)
    except tomllib.TOMLDecodeError as e:
        return [f"parse error: {e}"]

    meta = data.get("_meta")
    if not isinstance(meta, dict) or not str(meta.get("iso639", "")).strip():
        errors.append("missing [_meta] table with a non-empty 'iso639' code")

    present = set()
    for key, tbl in data.items():
        if key.startswith("_"):
            continue
        if not isinstance(tbl, dict):
            errors.append(f"'{key}' is not a [table]")
            continue
        if key not in source:
            errors.append(f"unknown key '{key}' (not in strings.toml)")
            continue
        present.add(key)
        # Anti-drift: the english reference must still equal the source, else the translation is stale.
        eng = tbl.get("english")
        if eng is None:
            errors.append(f"key '{key}': missing 'english' reference (run sync)")
        elif eng != source[key].english:
            errors.append(f"key '{key}': stale — source English changed since translation (re-translate)")
        tr = tbl.get("translation")
        if tr is None:
            errors.append(f"key '{key}': missing 'translation' field")
        elif tr.strip() == "":
            errors.append(f"key '{key}': empty translation (incomplete)")
        elif placeholder_indices(tr) != source[key].ph:
            errors.append(f"key '{key}': translation placeholders {{N}} do not match the source")
    for key in source:
        if key not in present:
            errors.append(f"missing key '{key}'")
    return errors


def cmd_check(args: argparse.Namespace) -> int:
    source = load_source()
    paths = resolve_ids(args.ids) if args.ids else \
        sorted(p for p in SHIP_DIR.glob("*.toml") if not p.name.endswith(BATCH_SUFFIX))
    ok = True
    for p in paths:
        errors = validate_lang(source, p)
        if errors:
            ok = False
            shown = errors[: args.max_errors]
            for e in shown:
                print(f"{rel(p)}: {e}")
            if len(errors) > len(shown):
                print(f"{rel(p)}: ... and {len(errors) - len(shown)} more")
        else:
            print(f"{rel(p)}: OK ({len(source)} keys)")
    return 0 if ok else 1


def cmd_promote(args: argparse.Namespace) -> int:
    source = load_source()
    src_path = WIP_DIR / f"{args.id}.toml"
    if not src_path.exists():
        die(f"no wip file at {rel(src_path)} (promote moves a wip/ language into lang/)")
    errors = validate_lang(source, src_path)
    if errors:
        print(f"{rel(src_path)} is not ready to ship:")
        for e in errors[:20]:
            print(f"  {e}")
        if len(errors) > 20:
            print(f"  ... and {len(errors) - 20} more")
        print(f"Finish translating it (translations.py status {args.id}), then promote again.")
        return 1
    SHIP_DIR.mkdir(parents=True, exist_ok=True)
    dest = SHIP_DIR / f"{args.id}.toml"
    if dest.exists():
        die(f"{rel(dest)} already exists")
    src_path.replace(dest)
    batch = WIP_DIR / f"{args.id}.batch.toml"
    if batch.exists():
        batch.unlink()
    print(f"promoted {args.id}: {rel(src_path)} -> {rel(dest)} (now shipped & build-validated)")
    return 0


# ─────────────────────────────────────────────────────────────────────────────
# Helpers
# ─────────────────────────────────────────────────────────────────────────────

def lang_display_name(lang_id: str) -> str:
    base = lang_id[: -len(AI_SUFFIX)] if lang_id.endswith(AI_SUFFIX) else lang_id
    return KNOWN_LANGS.get(base, (base, base))[0]


def rel(path: Path) -> str:
    try:
        return str(path.relative_to(REPO))
    except ValueError:
        return str(path)


def die(msg: str) -> "None":
    print(f"translations.py: error: {msg}", file=sys.stderr)
    raise SystemExit(2)


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="translations.py", description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="command", required=True)

    s = sub.add_parser("status", help="per-language completion report")
    s.add_argument("ids", nargs="*", help="language ids (default: all)")
    s.set_defaults(func=cmd_status)

    s = sub.add_parser("sync", help="mirror source structure into language file(s)")
    s.add_argument("ids", nargs="*", help="language ids (default: all)")
    s.set_defaults(func=cmd_sync)

    s = sub.add_parser("new", help="create a work-in-progress stub for a language")
    s.add_argument("code", help="language code, e.g. de or zh-Hant")
    s.add_argument("--ai", action="store_true", help="add the _[AI] machine-translation suffix")
    s.add_argument("--name", help="display name for the file header")
    s.add_argument("--iso639", help="ISO 639 code for [_meta] (default: derived from code)")
    s.set_defaults(func=cmd_new)

    s = sub.add_parser("todo", help="write a batch of keys needing translation")
    s.add_argument("id", help="language id")
    s.add_argument("-o", "--out", help="output path (default: localization/wip/<id>.batch.toml)")
    s.set_defaults(func=cmd_todo)

    s = sub.add_parser("apply", help="merge a filled batch back into the language file")
    s.add_argument("id", help="language id")
    s.add_argument("-i", "--input", help="batch path (default: localization/wip/<id>.batch.toml)")
    s.set_defaults(func=cmd_apply)

    s = sub.add_parser("check", help="validate language file(s) like the build does")
    s.add_argument("ids", nargs="*", help="language ids (default: all in lang/)")
    s.add_argument("--max-errors", type=int, default=25, help="cap errors shown per file")
    s.set_defaults(func=cmd_check)

    s = sub.add_parser("promote", help="move a complete wip/ language into lang/")
    s.add_argument("id", help="language id")
    s.set_defaults(func=cmd_promote)

    return p


def main(argv: list[str]) -> int:
    # Translation data is inherently non-ASCII; force UTF-8 output so printing a
    # translation (or an endonym) doesn't crash on Windows' legacy cp1252 console.
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="replace")
    args = build_parser().parse_args(argv[1:])
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
