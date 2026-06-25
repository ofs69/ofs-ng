"""Ad-hoc coverage gap report: prints missed line ranges per source file.

Usage: python tools/cov_report.py [substr ...]
  No args  -> top src files by missed-line count.
  Args     -> detailed missed line ranges for files whose basename contains any substr.

Point it at a different Cobertura report with OFS_COV_XML, e.g.
  OFS_COV_XML=cmake-build-debug-visual-studio/coverage/coverage.xml python tools/cov_report.py
"""
import os
import sys
import xml.etree.ElementTree as ET

XML = os.environ.get("OFS_COV_XML", "cmake-build-debug-visual-studio/coverage/coverage.xml")


def load():
    root = ET.parse(XML).getroot()
    agg = {}
    for c in root.iter("class"):
        fn = c.get("filename")
        lines = c.find("lines")
        if lines is None:
            continue
        d = agg.setdefault(fn, {})
        for ln in lines.findall("line"):
            n = int(ln.get("number"))
            d[n] = d.get(n, 0) + int(ln.get("hits"))
    return agg


def ranges(nums):
    groups = []
    for n in nums:
        if groups and n == groups[-1][-1] + 1:
            groups[-1].append(n)
        else:
            groups.append([n])
    return ", ".join(f"{g[0]}-{g[-1]}" if len(g) > 1 else str(g[0]) for g in groups)


def main():
    agg = load()
    subs = sys.argv[1:]
    if not subs:
        rows = []
        for fn, d in agg.items():
            norm = fn.replace("\\", "/")
            if "/src/" not in norm and not norm.lower().startswith("src"):
                continue
            nl = len(d)
            hit = sum(1 for v in d.values() if v > 0)
            rows.append((nl - hit, hit, nl, fn.split("\\")[-1]))
        rows.sort(key=lambda x: -x[0])
        for miss, hit, nl, base in rows[:40]:
            print(f"{miss:5d} miss  {hit:5d}/{nl:5d}  {100*hit/nl:5.1f}%  {base}")
        return
    for fn, d in sorted(agg.items()):
        base = fn.split("\\")[-1]
        if not any(s in base for s in subs):
            continue
        missed = sorted(n for n, h in d.items() if h == 0)
        if not missed:
            continue
        print(f"=== {base}: {len(missed)} missed of {len(d)} ===")
        print("  " + ranges(missed))


if __name__ == "__main__":
    main()
