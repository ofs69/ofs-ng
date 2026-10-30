"""Build a local slider gallery and pixel-difference report for two UI capture sets.

Requires Pillow. Screenshots stay local and are linked from the generated HTML.
"""

import argparse
import html
import json
import os
from pathlib import Path
from urllib.parse import quote

from PIL import Image, ImageChops, ImageStat


def image_link(path, output):
    return html.escape(quote(Path(os.path.relpath(path, output)).as_posix(), safe="/"), quote=True)


def compare(before, after, output):
    output.mkdir(parents=True, exist_ok=True)
    diff_dir = output / "diffs"
    diff_dir.mkdir(exist_ok=True)
    before_manifest = json.loads((before / "manifest.json").read_text(encoding="utf-8"))
    after_manifest = json.loads((after / "manifest.json").read_text(encoding="utf-8"))
    for key in ("display_size", "framebuffer_scale", "font_size_base", "font_scale_dpi", "renderer"):
        if before_manifest[key] != after_manifest[key]:
            raise ValueError(f"Capture configuration differs: {key}")
    if (before / "patterns.json").exists() or (after / "patterns.json").exists():
        before_patterns = json.loads((before / "patterns.json").read_text(encoding="utf-8"))
        after_patterns = json.loads((after / "patterns.json").read_text(encoding="utf-8"))
        if before_patterns != after_patterns:
            raise ValueError("Timeline pattern, zoom, or line-width settings differ")
    before_names = {p.name for p in before.glob("*.png")}
    after_names = {p.name for p in after.glob("*.png")}
    if not before_names or before_names != after_names:
        raise ValueError(f"Screenshot sets differ: {sorted(before_names ^ after_names)}")
    sections = []
    results = []
    for name in sorted(before_names):
        with Image.open(before / name) as source, Image.open(after / name) as candidate:
            if source.size != candidate.size:
                raise ValueError(f"Image dimensions differ: {name}")
            a, b = source.convert("RGB"), candidate.convert("RGB")
        difference = ImageChops.difference(a, b)
        r, g, blue = difference.split()
        changed = ImageChops.lighter(ImageChops.lighter(r, g), blue)
        unchanged = changed.histogram()[0]
        fraction = 1.0 - unchanged / (a.width * a.height)
        result = {"file": name, "size": a.size, "changed_percent": fraction * 100,
                  "mean_channel_difference": sum(ImageStat.Stat(difference).mean) / 3,
                  "difference_bounds": difference.getbbox()}
        results.append(result)
        difference.point(lambda value: min(255, value * 4)).save(diff_dir / name)
        label = html.escape(name)
        before_url = image_link(before / name, output)
        after_url = image_link(after / name, output)
        diff_url = image_link(diff_dir / name, output)
        sections.append(f'''<section><h2>{label}</h2>
<p>{fraction * 100:.2f}% of pixels changed · mean channel difference {result['mean_channel_difference']:.2f}/255</p>
<div class="comparison" style="aspect-ratio:{a.width}/{a.height}">
<img src="{after_url}" alt="After: {label}" loading="lazy">
<img class="before" src="{before_url}" alt="Before: {label}" loading="lazy">
</div><label>After <input type="range" min="0" max="100" value="50"
oninput="this.parentElement.previousElementSibling.style.setProperty('--split',this.value+'%')"> Before</label>
<p><a href="{before_url}">Before at full resolution</a> · <a href="{after_url}">After at full resolution</a> ·
<a href="{diff_url}">Difference ×4</a></p></section>''')
    report = {"before": before_manifest, "after": after_manifest, "images": results}
    (output / "comparison.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    title = f"ImGui {before_manifest['imgui_version']} → {after_manifest['imgui_version']}"
    document = f'''<!doctype html><meta charset="utf-8"><title>{html.escape(title)}</title>
<style>body{{background:#17191d;color:#edf1f7;font:16px system-ui;margin:24px auto;max-width:1920px;padding:0 20px}}
h1{{font-size:28px}}h2{{font-size:18px}}section{{margin:36px 0;border-top:1px solid #4e5561;padding-top:18px}}
a{{color:#8ccfff}}p{{color:#b8c2d1}}.comparison{{position:relative;width:100%;--split:50%;background:#111}}
.comparison img{{position:absolute;width:100%;height:100%;object-fit:contain}}
.comparison .before{{clip-path:inset(0 calc(100% - var(--split)) 0 0)}}
input{{width:min(70vw,700px);vertical-align:middle}}label{{display:block;margin-top:12px}}
</style><h1>{html.escape(title)}</h1><p>{len(results)} matching app views. Drag sliders to reveal the
before image over the after image. Pixel counts include expected rendering changes, live counters,
and timestamps; they are review aids, not pass/fail thresholds.</p>{''.join(sections)}'''
    (output / "index.html").write_text(document, encoding="utf-8")
    print(f"Compared {len(results)} images: {output / 'index.html'}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("before", type=Path)
    parser.add_argument("after", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    compare(args.before.resolve(), args.after.resolve(), args.output.resolve())


if __name__ == "__main__":
    main()
