"""Ad-hoc managed (C#) coverage gap report: prints missed line ranges per source file.

The native sibling tools/cov_report.py reads OpenCppCoverage output; this reads the
dotnet-coverage cobertura the `coverage-managed` CMake target produces (OpenCppCoverage
cannot see managed IL). Scope is the three host assemblies that target instruments:
Ofs.Api / Ofs.PluginHost / Ofs.ScriptHost.

Usage: python tools/cov_managed_report.py [substr ...]
  No args  -> per-assembly summary, then top files by missed-line count.
  Args     -> detailed missed line ranges for files whose basename contains any substr.
"""
import os
import sys
import xml.etree.ElementTree as ET

XML = "cmake-build-debug-visual-studio/coverage-managed/coverage.cobertura.xml"


def load():
    """Aggregate hits per (file, line) across classes; a file holds many C# classes, and a
    line counts as covered if any class hit it. Also map each file to its assembly."""
    root = ET.parse(XML).getroot()
    agg = {}
    pkg_of = {}
    for pkg in root.iter("package"):
        name = pkg.get("name")
        for c in pkg.iter("class"):
            fn = c.get("filename")
            lines = c.find("lines")
            if lines is None:
                continue
            pkg_of[fn] = name
            d = agg.setdefault(fn, {})
            for ln in lines.findall("line"):
                n = int(ln.get("number"))
                d[n] = d.get(n, 0) + int(ln.get("hits"))
    return agg, pkg_of


def ranges(nums):
    groups = []
    for n in nums:
        if groups and n == groups[-1][-1] + 1:
            groups[-1].append(n)
        else:
            groups.append([n])
    return ", ".join(f"{g[0]}-{g[-1]}" if len(g) > 1 else str(g[0]) for g in groups)


def main():
    if not os.path.exists(XML):
        sys.exit(f"no managed report at {XML} — run: cmake --build <build-dir> --target coverage-managed")
    agg, pkg_of = load()
    subs = sys.argv[1:]
    if not subs:
        # Per-assembly summary first (deduped per file+line, so it matches the file rows below).
        per_pkg = {}
        for fn, d in agg.items():
            hit = sum(1 for v in d.values() if v > 0)
            acc = per_pkg.setdefault(pkg_of.get(fn, "?"), [0, 0])
            acc[0] += hit
            acc[1] += len(d)
        for name, (hit, nl) in sorted(per_pkg.items()):
            print(f"{nl - hit:5d} miss  {hit:5d}/{nl:5d}  {100 * hit / nl if nl else 0:5.1f}%  {name}")
        print("-" * 48)
        rows = []
        for fn, d in agg.items():
            nl = len(d)
            hit = sum(1 for v in d.values() if v > 0)
            rows.append((nl - hit, hit, nl, os.path.basename(fn.replace("\\", "/"))))
        rows.sort(key=lambda x: -x[0])
        for miss, hit, nl, base in rows[:40]:
            print(f"{miss:5d} miss  {hit:5d}/{nl:5d}  {100 * hit / nl if nl else 0:5.1f}%  {base}")
        return
    for fn, d in sorted(agg.items()):
        base = os.path.basename(fn.replace("\\", "/"))
        if not any(s in base for s in subs):
            continue
        missed = sorted(n for n, h in d.items() if h == 0)
        if not missed:
            continue
        print(f"=== {base}: {len(missed)} missed of {len(d)} ===")
        print("  " + ranges(missed))


if __name__ == "__main__":
    main()
