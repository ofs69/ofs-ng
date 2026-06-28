# Translating ofs-ng

Every user-visible string lives once in `tools/localization/strings.toml` (the English source of truth)
and is reached through the generated `Str::`/`Tr` API — see the *Localized strings* section of
[`CLAUDE.md`](../CLAUDE.md) for the call-site rules. This document covers the other half: keeping the
per-language catalogs in step with that source and adding new languages, all through one tool —
**`tools/translations.py`** — instead of hand-editing a dozen 3,000-line TOML files.

## Where catalogs live

Every catalog is a `lang/<id>.toml` file. There is no staging area: a catalog is **shipped and strictly
validated by the build** (`localization_gen --validate`) the moment it exists. A file must be complete —
every source key present with a non-empty translation whose `{N}` placeholders match the source — or it
**fails the build.** A freshly added language therefore breaks the build until it is fully translated, so
add and translate a language in one pass (the batch loop below makes that quick).

The language **id** is the filename stem. Machine translations carry an `_[AI]` suffix
(`de_[AI].toml`), shown verbatim in the in-app language picker so users can tell them from
human-reviewed catalogs. Each file declares its BCP 47 culture tag in `[_meta].culture` — e.g. `de`,
`ja`, or a script/region subtag like `zh-Hant`, `zh-Hans`, `pt-BR` (handed to plugins, which feed it
straight to .NET `CultureInfo`, so they localize to match).

## File schema

Each key mirrors the source and adds a `translation`:

```toml
[PrefTitle]
english     = "Preferences"   # read-only reference — used to detect a stale translation
translation = "Einstellungen" # the only hand-authored field
```

`english` is **not** loaded at runtime; it is a reference. When the source English for a key changes,
the stored `english` no longer matches and the translation is flagged **stale** (likely out of date)
until it is re-translated. An empty/whitespace `translation` falls back to English at runtime.

## No drift (enforced by the build)

A translation drifts when the source it was made against changes but the translation does not — it then
ships silently outdated. To make that impossible, `localization_gen --validate` (run on every build over
`lang/*.toml`) **fails the build** on any of:

- a missing key, or a key not in the source (added/removed string);
- an empty translation;
- `{N}` placeholders that don't match the source;
- a **stale** key — the stored `english` reference no longer equals the source English.

So a shipped language is always complete *and* current: editing an English source string breaks the
build for every catalog that translated it, until each is refreshed. `python tools/translations.py check`
reproduces these exact checks without a build (use it in CI or pre-commit).

The refresh loop after a source-text change: `sync` (flags the stale keys) → `todo` (the batch
pre-fills each stale key's existing translation next to the new English, to revise in place) → `apply`
(re-stamps the `english` reference, clearing the stale flag).

## The tool

Run from anywhere in the repo. `<id>` is a filename stem like `de_[AI]`.

```
python tools/translations.py status [id ...]   # completion report (done/missing/empty/stale/badph)
python tools/translations.py sync   [id ...]   # propagate source changes into language file(s)
python tools/translations.py new    <code>     # create a stub language file in lang/
python tools/translations.py todo   <id>       # write a batch of just-the-keys-needing-work
python tools/translations.py apply  <id>       # merge a filled batch back in (validates placeholders)
python tools/translations.py check  [id ...]   # validate like the build, without building
```

### After you edit `strings.toml`

Run **`sync`** (no id = every language). It adds new keys (empty translation), drops keys you removed,
refreshes the English reference of untranslated keys, and preserves every existing translation. This is
the one command that replaces hand-editing every language file. A `sync` that adds keys warns you for
each language it touched — those languages are now incomplete and will fail the build until the new keys
are translated.

### Translating (the Claude-friendly loop)

You never have to open the full catalog. Work one language at a time through a focused batch:

1. `python tools/translations.py todo de_[AI]` — writes `tools/localization/de_[AI].batch.toml`
   containing only the keys that need work, each with its English text, translator `# context:` (the
   source `description`), and `# placeholder:` docs.
2. Fill in each `translation = ""` in that batch. Keep every `{N}` placeholder from the source (order
   is free). Leave a key blank to skip it for now.
3. `python tools/translations.py apply de_[AI]` — merges the batch back, **rejecting** any translation
   whose placeholders don't match the source, and clearing the stale flag on the keys it accepts.
4. `python tools/translations.py status de_[AI]` — repeat from step 1 until `done` is 100 %.

> Asking Claude Code to translate a language is just: run `todo`, fill the batch, run `apply`. Batch
> files are git-ignored and transient.

### Checking a language

```
python tools/translations.py check de_[AI]     # confirm complete + valid (mirrors the build check)
```

Once it is complete, build and run the suite — the `ui-smoke-loc` test re-runs the whole UI under a
machine translation to catch any label that lost its `###id`.

## Adding a language

```
python tools/translations.py new de --ai       # -> lang/de_[AI].toml, all keys empty
```

`new` derives the display name and BCP 47 culture tag for common languages automatically; override with
`--name` / `--culture`, and drop `--ai` for a human-authored catalog. The new file fails the build until
it is complete, so translate it via the loop above before committing.

## Which languages?

The shipped set targets the largest user bases for a tool like this: Simplified Chinese, Spanish,
German, French, Russian, Brazilian Portuguese, Korean, Italian, Polish, Turkish, and Traditional
Chinese. Japanese already ships. Add any other with `new`.
