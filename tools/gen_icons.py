"""
Generate PNG icons and a .ico file for ofs-ng.

Design: a free-floating desktop icon (transparent background, no tile) — the lucide
`audio-waveform` glyph (the same mark shown at the start of the title bar, ICON_APP in
src/UI/Icons.h) over a two-pass accent glow. Large icons (>= LABEL_MIN_SIZE) carry an
'ofs-ng' label beneath the mark; smaller icons drop the label and enlarge the glyph to fill
the square, since a baked-in label is an illegible smear once downscaled to 16/32 px.

Alongside the labeled `logo<size>.png` set, the tool also emits a title-less `logomark<size>.png`
set — the same label-less, square-filling master used for the small icons, rendered at large sizes
for places that want the bare symbol without the 'ofs-ng' wordmark (e.g. the docs-site navbar).

The colors are derived from the theme seed: gen_theme.py writes data/themes/palette.json,
and this tool reads it so the icon and the app's dark theme share one origin (the seed is
the accent; the background/grid/border come from the dark surface ramp). If palette.json is
absent, it falls back to the baked-in constants below. Regenerate the theme first to refresh:

    python tools/gen_theme.py            # writes palette.json from the seed
    python tools/gen_icons.py            # icon picks up the new palette

Run from any directory — output always goes to data/icons/ under the project root:

    python tools/gen_icons.py
    python tools/gen_icons.py --sizes 16 32 48 64 128 256 --no-ico
    python tools/gen_icons.py --mark-sizes 256 512        # title-less logomark sizes
    python tools/gen_icons.py --master-size 2048
"""

import argparse
import json
import sys
from pathlib import Path

try:
    from PIL import Image, ImageDraw, ImageFont, ImageFilter
except ImportError:
    sys.exit("Pillow is required: pip install Pillow")

PROJECT_ROOT = Path(__file__).resolve().parent.parent
FONT_PATH    = PROJECT_ROOT / "data" / "fonts" / "JetBrainsMono-Regular.ttf"
ICON_FONT_PATH = PROJECT_ROOT / "data" / "fonts" / "lucide.ttf"
ICONS_DIR    = PROJECT_ROOT / "data" / "icons"
PALETTE_PATH = PROJECT_ROOT / "data" / "themes" / "palette.json"

# Keep in sync with ICON_APP in src/UI/Icons.h: lucide `audio-waveform` (U+E55B). The icon
# and the title-bar app mark render the same glyph from the same font.
APP_GLYPH = ""

DEFAULT_SIZES = [16, 32, 48, 64, 128, 256, 512, 1024]
# Title-less logomark sizes. Only large sizes are useful here: below LABEL_MIN_SIZE the regular
# logo<size>.png is already label-less, so a separate mark would be identical.
DEFAULT_MARK_SIZES = [128, 256, 512]
ICO_SIZES     = [(16, 16), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)]

# Below this px size the 'ofs-ng' label is dropped (and the glyph enlarged to fill the square);
# at and above it the label is baked in. Matches the smallest size the label stays readable.
LABEL_MIN_SIZE = 64

# Fallback accent (used only if data/themes/palette.json is missing).
ACCENT = (79, 195, 247, 255)   # #4FC3F7


def _hex_rgba(s):
    s = s.lstrip("#")
    return (int(s[0:2], 16), int(s[2:4], 16), int(s[4:6], 16), 255)


def _load_accent():
    """The brand seed color from data/themes/palette.json; None if absent."""
    try:
        pal = json.loads(PALETTE_PATH.read_text(encoding="utf-8"))
        return _hex_rgba(pal["seed"])
    except (OSError, ValueError, KeyError):
        return None


_accent = _load_accent()
if _accent:
    ACCENT = _accent


# ---------------------------------------------------------------------------
# Glyph rendering (lucide audio-waveform)
# ---------------------------------------------------------------------------

def _glyph_sprite(px, color, stroke=0):
    """
    Render APP_GLYPH at `px` em-size in `color` and return it tightly cropped to its
    inked pixels. Cropping lets callers center the glyph by its actual ink box (font
    metrics leave asymmetric ascender/descender padding, so anchor-centering drifts).
    `stroke` fattens the outline — used to build the glow passes.
    """
    font  = _load_icon_font(px)
    box   = px * 3   # generous canvas so a stroked/blurred glyph never clips before cropping
    layer = Image.new("RGBA", (box, box), (0, 0, 0, 0))
    ImageDraw.Draw(layer).text(
        (box / 2, box / 2), APP_GLYPH, font=font, fill=color, anchor="mm",
        stroke_width=stroke, stroke_fill=color,
    )
    bbox = layer.getbbox()
    return layer.crop(bbox) if bbox else layer


def _paste_centered(canvas, sprite, cx, cy):
    """Composite `sprite` onto `canvas` so its ink box is centered at (cx, cy)."""
    canvas.alpha_composite(sprite, (int(round(cx - sprite.width / 2.0)),
                                    int(round(cy - sprite.height / 2.0))))


def _glow_layer(size, px, color_rgb, alpha, stroke, blur_radius, cx, cy):
    """Full-size layer with the (optionally fattened) glyph centered at (cx, cy), blurred.
    Built at full size so the gaussian halo isn't clipped at the sprite edge."""
    layer  = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    sprite = _glyph_sprite(px, (*color_rgb, alpha), stroke=stroke)
    _paste_centered(layer, sprite, cx, cy)
    return layer.filter(ImageFilter.GaussianBlur(radius=max(1, blur_radius)))


# ---------------------------------------------------------------------------
# Icon composition
# ---------------------------------------------------------------------------

def create_master_icon(size: int, *, with_label: bool = True) -> Image.Image:
    # Desktop icon: a free-floating waveform symbol on a transparent background — no tile,
    # border or grid. The symbol fills most of the canvas; the OS draws the filename below.
    margin = size * 0.07
    cx     = size / 2.0

    canvas = Image.new("RGBA", (size, size), (0, 0, 0, 0))

    # ── 1. Glyph geometry ──────────────────────────────────────────────────────
    # Reserve the bottom fraction for the baked-in 'ofs-ng' label. Small icons render label-less
    # (with_label=False) so the mark fills the whole square and stays legible once downscaled — a
    # full-canvas 'ofs-ng' shrunk to 16/32 px is an unreadable smear.
    text_reserve = size * 0.20 if with_label else 0.0

    wave_x0  = margin
    wave_x1  = size - margin
    wave_top = margin
    wave_bot = size - margin - text_reserve
    wave_cy  = (wave_top + wave_bot) / 2.0
    region_w = wave_x1 - wave_x0
    region_h = wave_bot - wave_top

    # Fit the glyph to the region: render once at a probe size to read its (fixed) aspect
    # ratio, then scale the em-size so the ink box fills the region with a little margin.
    probe     = max(8, int(region_h))
    probe_spr = _glyph_sprite(probe, ACCENT)
    scale     = min(region_w / probe_spr.width, region_h / probe_spr.height) * 0.98
    glyph_px  = max(8, int(probe * scale))
    line_w    = max(2, glyph_px // 26)

    # ── 2. Glyph glow (two passes for depth) ───────────────────────────────────
    accent_rgb = ACCENT[:3]

    # Soft outer halo. The glyph nearly fills the canvas, so an unbounded stroke + blur pushed the
    # gaussian tail past the canvas edge, where it clipped to a hard line. Cap the halo's reach to
    # the headroom (`margin`) so it always fades to ~0 inside the canvas — this also keeps the
    # label-less small-icon master safe, whose larger glyph would otherwise re-clip. _assert_no_-
    # border_clip() in main() guards the result.
    halo_stroke = min(line_w + glyph_px // 24, max(2, int(margin * 0.45)))
    halo_blur   = min(size // 14, max(4, int(margin * 0.32)))
    outer = _glow_layer(
        size, glyph_px, accent_rgb,
        alpha=40,
        stroke=halo_stroke,
        blur_radius=halo_blur,
        cx=cx, cy=wave_cy,
    )
    canvas = Image.alpha_composite(canvas, outer)

    # Tight, brighter inner glow
    inner = _glow_layer(
        size, glyph_px, accent_rgb,
        alpha=100,
        stroke=max(1, glyph_px // 40),
        blur_radius=max(2, size // 38),
        cx=cx, cy=wave_cy,
    )
    canvas = Image.alpha_composite(canvas, inner)

    # ── 3. Sharp glyph ─────────────────────────────────────────────────────────
    # Dark shadow first so the symbol stays legible on light wallpapers, then the accent glyph.
    shadow_off = max(1, int(size * 0.012))
    shadow_spr = _glyph_sprite(glyph_px, (0, 0, 0, 130))
    _paste_centered(canvas, shadow_spr, cx + shadow_off, wave_cy + shadow_off)
    _paste_centered(canvas, _glyph_sprite(glyph_px, ACCENT), cx, wave_cy)
    draw = ImageDraw.Draw(canvas)

    # ── 4. Label ───────────────────────────────────────────────────────────────
    if with_label:
        font     = _load_font(int(size * 0.205))
        text     = "ofs-ng"
        label_cy = (wave_bot + (size - margin)) / 2.0 + size * 0.02
        stroke   = max(1, int(size * 0.009))
        draw.text((cx, label_cy), text, fill=ACCENT, font=font, anchor="mm",
                  stroke_width=stroke, stroke_fill=(0, 0, 0, 130))

    return canvas


def _load_font(size: int) -> ImageFont.FreeTypeFont:
    if FONT_PATH.exists():
        try:
            return ImageFont.truetype(str(FONT_PATH), size)
        except Exception as exc:
            print(f"  Warning: font load failed ({exc}); using default")
    try:
        return ImageFont.load_default(size=size)
    except TypeError:
        return ImageFont.load_default()


def _load_icon_font(size: int) -> ImageFont.FreeTypeFont:
    if not ICON_FONT_PATH.exists():
        sys.exit(f"Icon font not found: {ICON_FONT_PATH}")
    return ImageFont.truetype(str(ICON_FONT_PATH), size)


def _assert_no_border_clip(img: Image.Image, *, max_alpha: int = 4) -> None:
    """Fail the build if the glow/shadow is clipped at a canvas edge — a hard line where the halo
    runs off the image instead of fading out. Every border pixel must be below an imperceptible
    alpha. Catches the exact regression the margin-capped halo (create_master_icon) prevents."""
    px = img.load()
    w, h = img.size
    edge = max(
        max(px[x, 0][3]     for x in range(w)),   # top
        max(px[x, h - 1][3] for x in range(w)),   # bottom
        max(px[0, y][3]     for y in range(h)),   # left
        max(px[w - 1, y][3] for y in range(h)),   # right
    )
    if edge > max_alpha:
        sys.exit(f"Border clip: edge alpha {edge} > {max_alpha}. The glow/shadow reaches the canvas "
                 f"edge and will show a hard line. Reduce the halo stroke/blur in create_master_icon.")


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def parse_args():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--sizes", nargs="+", type=int, default=DEFAULT_SIZES,
                   metavar="N",
                   help="PNG sizes to export (default: 16 32 48 64 128 256 512 1024)")
    p.add_argument("--mark-sizes", nargs="+", type=int, default=DEFAULT_MARK_SIZES,
                   metavar="N",
                   help="Title-less logomark<N>.png sizes (default: 128 256 512); "
                        "pass none to skip")
    p.add_argument("--no-ico", action="store_true",
                   help="Skip .ico file generation")
    p.add_argument("--master-size", type=int, default=1024, metavar="N",
                   help="Internal render resolution (default: 1024); "
                        "use 2048 for crisper downsamples")
    return p.parse_args()


def main():
    args = parse_args()
    ICONS_DIR.mkdir(parents=True, exist_ok=True)

    # Two masters: the labeled mark for large icons, a label-less mark (glyph enlarged to fill the
    # square) for small icons where a baked-in label would be illegible. Each output size downsamples
    # from the appropriate one.
    print(f"Rendering masters at {args.master_size}×{args.master_size}…")
    master_labeled = create_master_icon(args.master_size, with_label=True)
    master_plain   = create_master_icon(args.master_size, with_label=False)
    for m in (master_labeled, master_plain):
        _assert_no_border_clip(m)

    def frame(size: int) -> Image.Image:
        src = master_labeled if size >= LABEL_MIN_SIZE else master_plain
        return src.resize((size, size), Image.Resampling.LANCZOS)

    for size in sorted(set(args.sizes)):
        out = ICONS_DIR / f"logo{size}.png"
        print(f"  {out.relative_to(PROJECT_ROOT)}")
        frame(size).save(out)

    # Title-less logomark set: always the label-less master, downscaled — the bare symbol for
    # uses that supply their own wordmark or have no room for one (e.g. the docs-site navbar).
    for size in sorted(set(args.mark_sizes)):
        out = ICONS_DIR / f"logomark{size}.png"
        print(f"  {out.relative_to(PROJECT_ROOT)}")
        master_plain.resize((size, size), Image.Resampling.LANCZOS).save(out)

    if not args.no_ico:
        out = ICONS_DIR / "ofs.ico"
        print(f"  {out.relative_to(PROJECT_ROOT)}")
        # Embed each size from its own (labeled or plain) frame rather than resampling one base.
        # Pillow drops requested sizes larger than the base image, so the base must be the largest.
        frames = [frame(w) for w, _ in sorted(ICO_SIZES, reverse=True)]
        frames[0].save(str(out), format="ICO", sizes=ICO_SIZES, append_images=frames[1:])

    print("Done.")


if __name__ == "__main__":
    main()
