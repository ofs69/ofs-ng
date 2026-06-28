#!/usr/bin/env python3
"""Guard against dead localization keys in strings.toml.

Every key in `tools/localization/strings.toml` becomes a `Tr` enum member and a `Str::`
constant (via `localization_gen`), and is reached from a render path through one of
those — `Str::Foo`, `Str::Foo.id(...)`, `Str::Foo.fmt(...)`, or the raw `Tr::Foo`.
A key that no source file references is dead weight: it still has to be translated in
every `lang/*.toml`, yet displays nowhere.

This script parses the key tables out of strings.toml, scans the C++ sources for
`Str::<Key>` / `Tr::<Key>` tokens, and reports any key that is never referenced.

A key is exempt if its `[Key]` header line carries the marker `# i18n-keep`
(for a key deliberately defined ahead of its use site).

Usage:
    python tools/check_unused_strings.py [strings.toml] [source-dir ...]
    # defaults: tools/localization/strings.toml, scanning src/ and tests/

Exit status is non-zero if any unused key is found, so it can gate a commit or be
wired into a build target.

NOTE: this matches the *explicit* `Str::`/`Tr::` reference forms. It does not resolve
a key reached purely by a runtime string name (none exist today — `keyToTr` is only
fed key names read back from a lang file). Mark any such intentional case `# i18n-keep`.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

SOURCE_SUFFIXES = {".cpp", ".cc", ".cxx", ".h", ".hpp", ".hh", ".inl"}
KEEP_MARKER = "i18n-keep"

# A top-level [Key] table header. Keys are valid C++ identifiers; the reserved
# metadata table ([_meta], lang files only) and any other "_"-prefixed table are skipped.
KEY_HEADER = re.compile(r"^\[([A-Za-z][A-Za-z0-9_]*)\]\s*(#.*)?$")

# A call-site reference: Str::Foo or Tr::Foo (covers .id()/.fmt()/.icon()/.c_str() too,
# since they all start with the Str::Foo / Tr::Foo token).
REFERENCE = re.compile(r"\b(?:Str|Tr)::([A-Za-z][A-Za-z0-9_]*)\b")


def parse_keys(strings_toml: Path) -> dict[str, int]:
    """Return {key: line number} for every non-exempt key table in strings.toml."""
    keys: dict[str, int] = {}
    for lineno, line in enumerate(strings_toml.read_text(encoding="utf-8").splitlines(), start=1):
        m = KEY_HEADER.match(line)
        if not m:
            continue
        if KEEP_MARKER in line:
            continue
        keys[m.group(1)] = lineno
    return keys


def iter_sources(roots: list[Path]):
    for root in roots:
        if root.is_file():
            if root.suffix in SOURCE_SUFFIXES:
                yield root
            continue
        for path in root.rglob("*"):
            if path.suffix in SOURCE_SUFFIXES and path.is_file():
                yield path


def collect_references(roots: list[Path]) -> set[str]:
    used: set[str] = set()
    for path in iter_sources(roots):
        try:
            text = path.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            text = path.read_text(encoding="utf-8", errors="replace")
        used.update(REFERENCE.findall(text))
    return used


def main(argv: list[str]) -> int:
    here = Path(__file__).resolve().parent.parent
    args = argv[1:]
    if args:
        strings_toml = Path(args[0])
        roots = [Path(a) for a in args[1:]] or [here / "src", here / "tests"]
    else:
        strings_toml = here / "tools" / "localization" / "strings.toml"
        roots = [here / "src", here / "tests"]

    keys = parse_keys(strings_toml)
    used = collect_references(roots)
    unused = sorted((k, ln) for k, ln in keys.items() if k not in used)

    for key, lineno in unused:
        rel = strings_toml
        try:
            rel = strings_toml.relative_to(here)
        except ValueError:
            pass
        print(f"{rel}:{lineno}: unused localization key '{key}' — no Str::{key} / Tr::{key} reference")

    if unused:
        print()
        print(f"FAIL: {len(unused)} unused localization key(s). "
              f"Remove the dead key(s) from strings.toml and every lang/*.toml, "
              f"or mark a deliberately-reserved key with '# {KEEP_MARKER}' on its [Key] line.")
        return 1

    print(f"OK: all {len(keys)} localization keys are referenced.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
