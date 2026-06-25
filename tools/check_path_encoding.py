#!/usr/bin/env python3
"""Guard against lossy path<->string conversions in the C++ sources.

On Windows `std::filesystem::path::string()` re-encodes to the active ANSI
codepage (dropping any character outside it), and the narrow `path(std::string)`
constructor / `operator/` / `operator+=` interpret their bytes as ANSI rather than
UTF-8. The whole codebase keeps paths as UTF-8 `std::string` and `std::filesystem::path`,
converting only through `ofs::util::toUtf8()` / `ofs::util::fromUtf8()` (see
src/Util/PathUtil.h).

This script flags the two conversions that are easy to grep for:
  * `path.string()`            -> use ofs::util::toUtf8(path)
  * `std::filesystem::path(s)` / `fs::path(s)` from a narrow string
                               -> use ofs::util::fromUtf8(s)

A line is exempt if it contains the marker `// utf8-ok` (use this for the
legitimate wide / u8string constructions that the regex cannot distinguish).

Usage:
    python tools/check_path_encoding.py [path ...]      # default: src/

Exit status is non-zero if any violation is found, so it can gate a commit
or be wired into a build target.

NOTE: this catches the common *explicit* conversions. It cannot see implicit
narrow conversions (e.g. passing a UTF-8 std::string straight to
std::filesystem::exists()). The convention in PathUtil.h still has to be followed
by hand for those; convert at the boundary with fromUtf8().
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

SOURCE_SUFFIXES = {".cpp", ".cc", ".cxx", ".h", ".hpp", ".hh", ".inl"}
ALLOW_MARKER = "utf8-ok"

# (compiled regex, human-readable message) pairs.
RULES: list[tuple[re.Pattern[str], str]] = [
    (
        re.compile(r"\.string\(\)"),
        "path::string() is lossy on Windows (ANSI codepage). Use ofs::util::toUtf8(path).",
    ),
    (
        re.compile(r"(?:std::filesystem|fs)::path\s*\("),
        "narrow path(std::string) is ANSI on Windows. Use ofs::util::fromUtf8(utf8) "
        "(or add '// utf8-ok' if the argument is a wide/u8string/path).",
    ),
]

# The helper definitions themselves legitimately mention these tokens.
SKIP_FILES = {"PathUtil.cpp", "PathUtil.h"}


def iter_sources(roots: list[Path]):
    for root in roots:
        if root.is_file():
            if root.suffix in SOURCE_SUFFIXES:
                yield root
            continue
        for path in root.rglob("*"):
            if path.suffix in SOURCE_SUFFIXES and path.is_file():
                yield path


def check_file(path: Path) -> list[tuple[int, str, str]]:
    violations: list[tuple[int, str, str]] = []
    try:
        text = path.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        text = path.read_text(encoding="utf-8", errors="replace")
    for lineno, line in enumerate(text.splitlines(), start=1):
        if ALLOW_MARKER in line:
            continue
        for pattern, message in RULES:
            if pattern.search(line):
                violations.append((lineno, line.strip(), message))
    return violations


def main(argv: list[str]) -> int:
    here = Path(__file__).resolve().parent.parent
    if len(argv) > 1:
        roots = [Path(a) for a in argv[1:]]
    else:
        roots = [here / "src"]

    total = 0
    for path in sorted(iter_sources(roots)):
        if path.name in SKIP_FILES:
            continue
        for lineno, snippet, message in check_file(path):
            total += 1
            rel = path
            try:
                rel = path.relative_to(here)
            except ValueError:
                pass
            print(f"{rel}:{lineno}: {message}")
            print(f"    {snippet}")

    if total:
        print()
        print(f"FAIL: {total} path-encoding violation(s). "
              f"Convert via ofs::util::toUtf8 / fromUtf8, or mark intentional "
              f"wide/u8 conversions with '// {ALLOW_MARKER}'.")
        return 1

    print("OK: no path-encoding violations found.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
