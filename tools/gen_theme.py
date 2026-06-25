"""
Generate ofs-ng base themes (dark + light) from a single seed color, Material-3-style.

One seed color -> tonal palettes (primary/secondary/tertiary/neutral/neutral-variant/
error) -> role tokens (surface, primary, outline, ...) -> mapped onto the three theme
namespaces ofs uses:

    ImGui    : ImGuiCol      + ImGuiStyleVar
    ofs      : AppCol         + AppVar           (extend the ImGui enums)
    imnodes  : ImNodesCol     + ImNodesStyleVar

The C++ side only LOADS the JSON this tool emits; all tonal-palette math lives here.

Outputs (under data/themes/):
    dark.json, light.json   - full Theme JSON (the shipped base themes). These are packed into the
                              shipped assets.zip at build time (cmake/PackAssets.cmake) and read at
                              runtime via ofs::res, so a regenerate only takes effect after a rebuild.
    palette.json            - resolved role tokens + tonal ramps for both schemes
                              (shared source of truth; tools/gen_icons.py reads it)

Run from any directory:

    python tools/gen_theme.py
    python tools/gen_theme.py --seed "#EE935C"
"""

import argparse
import colorsys
import json
import math
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent
THEMES_DIR = PROJECT_ROOT / "data" / "themes"
# dark/light.json are packed into assets.zip at build time (cmake/PackAssets.cmake), so the
# source files under data/themes are the single source of truth — a rebuild re-packs them.

# The default seed is shared with the app icon (gen_icons.py reads palette.json), so
# dark/light/app-icon all originate from one color. Amber: warm accent over cool graphite chrome.
DEFAULT_SEED = "#F2A33C"

SCHEMA_VERSION = 2

# ---------------------------------------------------------------------------
# Color science: sRGB <-> linear <-> XYZ <-> CIELAB <-> LCh, with gamut mapping.
# "Tone" (Material 3) is approximated by CIELAB L* (0..100). Full HCT/CAM16 can
# replace this later without changing the JSON contract.
# ---------------------------------------------------------------------------

_D65 = (0.95047, 1.00000, 1.08883)


def _hex_to_srgb(h):
    h = h.lstrip("#")
    if len(h) == 3:
        h = "".join(c * 2 for c in h)
    return tuple(int(h[i : i + 2], 16) / 255.0 for i in (0, 2, 4))


def _srgb_to_hex(rgb):
    return "#" + "".join("%02X" % round(_clamp01(c) * 255) for c in rgb[:3])


def _clamp01(x):
    return 0.0 if x < 0.0 else (1.0 if x > 1.0 else x)


def _srgb_to_lin(c):
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def _lin_to_srgb(c):
    return 12.92 * c if c <= 0.0031308 else 1.055 * (c ** (1 / 2.4)) - 0.055


def _srgb_to_xyz(rgb):
    r, g, b = (_srgb_to_lin(c) for c in rgb)
    x = 0.4124564 * r + 0.3575761 * g + 0.1804375 * b
    y = 0.2126729 * r + 0.7151522 * g + 0.0721750 * b
    z = 0.0193339 * r + 0.1191920 * g + 0.9503041 * b
    return (x, y, z)


def _xyz_to_srgb(xyz):
    x, y, z = xyz
    r = 3.2404542 * x - 1.5371385 * y - 0.4985314 * z
    g = -0.9692660 * x + 1.8760108 * y + 0.0415560 * z
    b = 0.0556434 * x - 0.2040259 * y + 1.0572252 * z
    return tuple(_lin_to_srgb(c) for c in (r, g, b))


def _f(t):
    d = 6.0 / 29.0
    return t ** (1.0 / 3.0) if t > d**3 else t / (3 * d * d) + 4.0 / 29.0


def _f_inv(t):
    d = 6.0 / 29.0
    return t**3 if t > d else 3 * d * d * (t - 4.0 / 29.0)


def _srgb_to_lab(rgb):
    x, y, z = _srgb_to_xyz(rgb)
    fx, fy, fz = _f(x / _D65[0]), _f(y / _D65[1]), _f(z / _D65[2])
    return (116 * fy - 16, 500 * (fx - fy), 200 * (fy - fz))


def _lab_to_srgb(lab):
    L, a, b = lab
    fy = (L + 16) / 116
    fx = fy + a / 500
    fz = fy - b / 200
    xyz = (_f_inv(fx) * _D65[0], _f_inv(fy) * _D65[1], _f_inv(fz) * _D65[2])
    return _xyz_to_srgb(xyz)


def _in_gamut(rgb, eps=1e-4):
    return all(-eps <= c <= 1 + eps for c in rgb)


class TonalPalette:
    """A fixed hue + chroma; tone(0..100) varies CIELAB lightness with gamut mapping."""

    def __init__(self, hue_deg, chroma):
        self.hue = hue_deg
        self.chroma = chroma

    def tone(self, t):
        hr = math.radians(self.hue)
        c = self.chroma
        # Reduce chroma until the requested tone is representable in sRGB.
        for _ in range(64):
            lab = (t, c * math.cos(hr), c * math.sin(hr))
            rgb = _lab_to_srgb(lab)
            if _in_gamut(rgb):
                return tuple(_clamp01(x) for x in rgb)
            c *= 0.96
        return tuple(_clamp01(x) for x in _lab_to_srgb((t, 0, 0)))


# Tonal-palette chroma/hue spec. The single source of truth for palette construction:
# build_palettes() reads it. Change a value here and the generated JSON updates.
#
# The surfaces are GRAPHITE — a fixed cool neutral hue, deliberately DECOUPLED from the accent
# seed. This is the house identity: a warm accent (amber) reads against cool near-neutral chrome,
# giving the warm↔cool tension that makes the UI feel designed rather than tinted. Letting a warm
# seed bleed into the surfaces (the old behavior) muddied every panel brown; pinning the surface
# family to `surfaceHue` keeps panels clean graphite for ANY seed, so the seed only ever colors
# the accents. The chrome tiers (containers, headers, frames, title bar, popups, outlines) carry a
# few units of that cool chroma — felt, not seen. WindowBg and the deepest canvas tier (timeline /
# node-graph / sim backdrops) stay on the pure-neutral `neutralSurface` ramp (chroma 0): a big flat
# field amplifies any tint, so those read clean while identity lives on the panels + accents. An
# achromatic seed collapses the chromatic accent palettes to 0 (via `g`) for a clean grayscale UI.
#
# Accents form a TRIAD rather than three shades of one hue: warm amber primary (seed) + cool teal
# secondary + cool blue/periwinkle tertiary. Three distinct tones with warm↔cool contrast — the
# antidote to a single-hue, flat-feeling scheme. Both secondary/tertiary are kept well off the
# error red (hue 25) so neither ever reads as an error state.
PALETTE_PARAMS = {
    "primaryChroma": 56.0,        # primary: a rich (not neon) take on the seed hue — amber wants
                                  # more chroma than a red/blue seed to read as vivid amber
    "secondaryHueShift": 120.0,   # secondary -> teal (kept green of the blue tertiary so the cool
                                  # accents stay distinct: amber + teal + blue triad)
    "secondaryChroma": 46.0,
    "tertiaryHueShift": 190.0,    # tertiary -> cool blue/periwinkle, cohesive with the graphite surfaces
    "tertiaryChroma": 48.0,
    "surfaceHue": 258.0,          # GRAPHITE: cool neutral hue for all surface/chrome, seed-independent
    "neutralChroma": 4.0,         # panel/chrome family — subtle cool graphite tint
    "neutralVariantChroma": 6.0,  # outlines/dividers — a touch cooler than surfaces
    "errorHue": 25.0,             # error stays red even for a monochrome seed
    "errorChroma": 68.0,
    "successHue": 145.0,          # success stays green even for a monochrome seed (mirrors error)
    "successChroma": 52.0,
    "warningHue": 85.0,           # warning stays amber/yellow regardless of seed (between error red and
    "warningChroma": 70.0,        # success green, and clear of the warm primary so it reads as a state)
    "achromaticThreshold": 4.0,   # seed chroma below this -> grayscale accents
}


def build_palettes(seed_rgb):
    L, a, b = _srgb_to_lab(seed_rgb)
    hue = math.degrees(math.atan2(b, a)) % 360.0
    chroma = math.hypot(a, b)
    P = PALETTE_PARAMS
    sh = P["surfaceHue"]  # surfaces use a fixed cool graphite hue, NOT the (warm) accent seed hue
    # An achromatic (gray) seed has no meaningful hue; collapse the chromatic palettes to 0
    # so the scheme stays a clean grayscale instead of snapping to hue 0 (red).
    g = (lambda c: 0.0) if chroma < P["achromaticThreshold"] else (lambda c: c)
    return {
        "primary": TonalPalette(hue, g(P["primaryChroma"])),
        "secondary": TonalPalette((hue + P["secondaryHueShift"]) % 360.0, g(P["secondaryChroma"])),
        "tertiary": TonalPalette((hue + P["tertiaryHueShift"]) % 360.0, g(P["tertiaryChroma"])),
        "neutral": TonalPalette(sh, g(P["neutralChroma"])),
        "neutralSurface": TonalPalette(sh, 0.0),  # WindowBg + canvas tier: pure neutral (large flat fields)
        "neutralVariant": TonalPalette(sh, g(P["neutralVariantChroma"])),
        "error": TonalPalette(P["errorHue"], P["errorChroma"]),
        "success": TonalPalette(P["successHue"], P["successChroma"]),
        "warning": TonalPalette(P["warningHue"], P["warningChroma"]),
    }


# ---------------------------------------------------------------------------
# Schemes: role -> (palette, tone). Standard Material 3 tone assignments.
# ---------------------------------------------------------------------------

_DARK_ROLES = {
    # Accents sit on their high-chroma tones rather than the M3 dark default (80): for warm/blue
    # hues sRGB chroma peaks well below tone 80, so the tones below read far more vivid than the
    # pale tone-80 tints while staying bright enough to contrast on the deep surfaces. Amber peaks
    # higher in lightness than red/blue, so primary sits at 74 to land a bright, golden amber. Teal
    # (secondary) peaks at high lightness too, so it stays near 80 — darkening it would only mute it.
    "primary": ("primary", 74), "onPrimary": ("primary", 20),
    "primaryContainer": ("primary", 30), "onPrimaryContainer": ("primary", 90),
    "inversePrimary": ("primary", 40),
    "secondary": ("secondary", 78), "onSecondary": ("secondary", 20),
    "secondaryContainer": ("secondary", 30), "onSecondaryContainer": ("secondary", 90),
    "tertiary": ("tertiary", 62), "onTertiary": ("tertiary", 20),
    "tertiaryContainer": ("tertiary", 30), "onTertiaryContainer": ("tertiary", 90),
    "error": ("error", 80), "onError": ("error", 20),
    "errorContainer": ("error", 30), "onErrorContainer": ("error", 90),
    # Green peaks lighter than red in sRGB, so success sits a touch below error's tone 80 to stay
    # vivid (not pastel) while still reading bright on the deep surfaces.
    "success": ("success", 76),
    # Amber peaks brightest of the three status hues in sRGB, so warning sits a little lower again to
    # avoid washing out while staying vivid on the deep surfaces.
    "warning": ("warning", 80),
    # Floor lifted off pure black (lowest 4 / surface 6) so the large canvases — timeline,
    # node grid, sim view — read as a deep surface rather than a void. The chrome tiers are
    # spaced WIDER than the M3 dark reference (container 15 / high 20 / highest 26 / bright 30)
    # so stacked panels, frames and popups separate by value instead of looking like one slab.
    "surface": ("neutralSurface", 5), "onSurface": ("neutral", 96),
    "surfaceDim": ("neutralSurface", 5), "surfaceBright": ("neutral", 33),
    "surfaceContainerLowest": ("neutralSurface", 4), "surfaceContainerLow": ("neutral", 14),
    "surfaceContainer": ("neutral", 18), "surfaceContainerHigh": ("neutral", 23),
    "surfaceContainerHighest": ("neutral", 29),
    "surfaceVariant": ("neutralVariant", 32), "onSurfaceVariant": ("neutralVariant", 82),
    "outline": ("neutralVariant", 62), "outlineVariant": ("neutralVariant", 38),
    "inverseSurface": ("neutral", 90), "inverseOnSurface": ("neutral", 20),
    # A bright, theme-tinted near-white used for text drawn on the colored band fills. Same tone in
    # both schemes so band labels stay bright on dark AND light (it must read on a saturated fill,
    # not follow onSurface, which flips dark in the light scheme).
    "brightText": ("neutral", 96),
}

_LIGHT_ROLES = {
    "primary": ("primary", 40), "onPrimary": ("primary", 100),
    "primaryContainer": ("primary", 90), "onPrimaryContainer": ("primary", 10),
    "inversePrimary": ("primary", 80),
    "secondary": ("secondary", 40), "onSecondary": ("secondary", 100),
    "secondaryContainer": ("secondary", 90), "onSecondaryContainer": ("secondary", 10),
    "tertiary": ("tertiary", 40), "onTertiary": ("tertiary", 100),
    "tertiaryContainer": ("tertiary", 90), "onTertiaryContainer": ("tertiary", 10),
    "error": ("error", 40), "onError": ("error", 100),
    "errorContainer": ("error", 90), "onErrorContainer": ("error", 10),
    "success": ("success", 40),
    "warning": ("warning", 40),
    "surface": ("neutralSurface", 89), "onSurface": ("neutral", 10),
    "surfaceDim": ("neutralSurface", 78), "surfaceBright": ("neutralSurface", 89),
    "surfaceContainerLowest": ("neutralSurface", 94), "surfaceContainerLow": ("neutral", 87),
    "surfaceContainer": ("neutral", 85), "surfaceContainerHigh": ("neutral", 83),
    "surfaceContainerHighest": ("neutral", 81),
    "surfaceVariant": ("neutralVariant", 90), "onSurfaceVariant": ("neutralVariant", 30),
    "outline": ("neutralVariant", 50), "outlineVariant": ("neutralVariant", 80),
    "inverseSurface": ("neutral", 20), "inverseOnSurface": ("neutral", 95),
    "brightText": ("neutral", 96),  # see _DARK_ROLES: bright band-label text, identical in both schemes
}


def resolve_roles(palettes, role_table):
    return {name: palettes[pal].tone(t) + (1.0,) for name, (pal, t) in role_table.items()}


# ---------------------------------------------------------------------------
# Color ops on RGBA tuples (sRGB, 0..1). Mirrored in the preview's JS.
# ---------------------------------------------------------------------------

def mix(c1, c2, t):
    return tuple(c1[i] + (c2[i] - c1[i]) * t for i in range(4))


def alpha(c, a):
    return (c[0], c[1], c[2], a)


def state(base, on, op):
    """Material state layer: overlay `on` (onSurface) at opacity `op`, keep base alpha."""
    m = mix(base, on, op)
    return (m[0], m[1], m[2], base[3])


def brighten_axis(c, f=1.15):
    """Lift HSV value by f (keeping hue + saturation) so axis lines read a touch brighter
    against the deep surfaces, without washing out their identity."""
    h, s, v = colorsys.rgb_to_hsv(c[0], c[1], c[2])
    r, g, b = colorsys.hsv_to_rgb(h, s, min(1.0, v * f))
    return (r, g, b, c[3])


def _b(r, g, b, a=255):
    return (r / 255.0, g / 255.0, b / 255.0, a / 255.0)


# ---------------------------------------------------------------------------
# Seed-independent, meaning-carrying colors kept per scheme (axes encode L0=stroke=red
# etc.; node categories and 2D-sim colors are deliberate). Pulled from src/UI/Theme.cpp.
# These do NOT change with the seed, so the live picker leaves them fixed.
# ---------------------------------------------------------------------------

# Both schemes share one axis palette: the deeper, more saturated set. The old lighter dark-theme
# variants read too bright against the deep dark surfaces.
_AXIS_COLORS = [
    _b(220, 50, 50), _b(210, 130, 0), _b(190, 170, 0), _b(40, 160, 40), _b(0, 140, 160),
    _b(60, 60, 210), _b(130, 50, 210), _b(180, 50, 160), _b(120, 170, 40), _b(40, 170, 140),
    _b(130, 130, 130), _b(100, 125, 145), _b(145, 110, 110), _b(145, 135, 95), _b(105, 145, 105),
    _b(95, 140, 140), _b(125, 105, 145), _b(145, 120, 95), _b(100, 145, 125), _b(130, 115, 150),
]
_AXIS_NAMES = ["L0", "L1", "L2", "R0", "R1", "R2", "V0", "V1", "A0", "A1",
               "S0", "S1", "S2", "S3", "S4", "S5", "S6", "S7", "S8", "S9"]

# Node title / link category colors. Dimmed (~0.75-0.8x) so they read as quiet category
# tags against the deep neutral canvas — ProcessingPanel brighten()s them on hover/select.
# The Link* pair doubles as the functional/discrete entry TEXT in the add-node menu, and NodeMath
# (which has no bright Link* sibling) does the same for math. As menu text they must clear the dark
# popup background, so on the dark scheme they are lifted brighter than a pure title fill would need
# — matched in value to NodeMath, which already carries this dual role.
_NODE_CAT_DARK = {"NodeIO": _b(44, 68, 104), "NodeDiscrete": _b(118, 71, 16),
                  "NodeFunctional": _b(16, 86, 78), "NodeMath": _b(153, 107, 209),
                  "LinkDiscrete": _b(204, 142, 65), "LinkFunctional": _b(66, 189, 168)}
_NODE_CAT_LIGHT = {"NodeIO": _b(100, 145, 195), "NodeDiscrete": _b(200, 130, 40),
                   "NodeFunctional": _b(40, 155, 135), "NodeMath": _b(135, 95, 185),
                   "LinkDiscrete": _b(180, 105, 25), "LinkFunctional": _b(25, 135, 115)}

# 2D simulator bar: the fixed cyan/navy/red identity, shared by every theme (seed-independent,
# identical in dark + light). Mirrored in the code-only Retro themes (src/UI/Theme.cpp
# fillBaseAppColors) so the bar looks the same across all themes.
_SIM2D = {"Sim2DFront": _b(0x01, 0xBA, 0xEF), "Sim2DBack": _b(0x10, 0x10, 0x10, 0xBF),
          "Sim2DBorder": _b(0x0B, 0x4F, 0x6C), "Sim2DText": _b(255, 255, 255),
          "Sim2DLines": _b(0x0B, 0x4F, 0x6C), "Sim2DIndicator": _b(0xFF, 0x4F, 0x6C)}

# Tempo overlay measure (downbeat) line color; seed-independent like the axis set.
_TEMPO_MEASURE_COLOR = _b(0xE7, 0x97, 0x5C)

# Action-point identity on the timeline curve: a black contrast outline behind every action, a red
# point, switching to green when selected. The classic funscript-editor convention — seed- AND
# scheme-independent (the black outline reads on either canvas), so kept fixed like the axis set.
_TIMELINE_POINT = {"TimelineOutline": _b(0, 0, 0), "TimelinePoint": _b(255, 0, 0),
                   "TimelinePointSelected": _b(11, 252, 3)}

_HEATMAP_MARKS = [
    (0.0, _b(235, 235, 245)), (0.2, _b(0x1E, 0x90, 255)), (0.4, _b(0, 255, 255)),
    (0.6, _b(0, 255, 0)), (0.8, _b(255, 255, 0)), (1.0, _b(255, 0, 0)),
]


def embed_app(dark):
    """Seed-independent AppCol entries (axes, node categories, 2D sim, literals)."""
    axis = [brighten_axis(c) for c in _AXIS_COLORS]
    d = {}
    for i, n in enumerate(_AXIS_NAMES):
        d["Axis" + n] = axis[i]
    # AxisDim* are omitted: apply() always derives them from base axis colors.
    d["TextShadow"] = (0, 0, 0, 1) if dark else _b(25, 25, 25)
    d["DragPreviewOutline"] = (0, 0, 0, 1)
    d["SimTintTop"] = _b(140, 210, 140) if dark else _b(40, 130, 40)
    d["SimTintSide"] = _b(140, 140, 210) if dark else _b(40, 40, 130)
    d["BookmarkOutline"] = (0, 0, 0, 0.70) if dark else (1, 1, 1, 0.70)
    d.update(_NODE_CAT_DARK if dark else _NODE_CAT_LIGHT)
    d.update(_SIM2D)
    d.update(_TIMELINE_POINT)
    d["TempoMeasureLine"] = _TEMPO_MEASURE_COLOR
    return d


# ---------------------------------------------------------------------------
# Declarative mapping spec. Each value is an expression node:
#   "roleName"                       -> roles[roleName]
#   {"role": n, "alpha": a}          -> roles[n] with alpha a
#   {"lit": [r,g,b,a]}               -> literal
#   {"mix": [A, B, t]}               -> lerp(eval A, eval B, t)
#   {"state": [Base, On, op]}        -> Material state layer (overlay On at op)
#   {"alpha": [Expr, a]}             -> eval Expr with alpha a
# A nested expression may appear anywhere an operand is expected.
# ---------------------------------------------------------------------------

def _ra(n, a):       # role with alpha
    return {"role": n, "alpha": a}


def _lit(r, g, b, a=1.0):
    return {"lit": [r, g, b, a]}


def _mix(a, b, t):
    return {"mix": [a, b, t]}


def _state(base, on, op):
    return {"state": [base, on, op]}


def _alpha(expr, a):
    return {"alpha": [expr, a]}


def evaluate(node, R):
    if isinstance(node, str):
        return R[node]
    if "role" in node:
        c = R[node["role"]]
        return (c[0], c[1], c[2], node["alpha"]) if "alpha" in node else c
    if "lit" in node:
        l = node["lit"]
        return tuple(l) if len(l) == 4 else tuple(l) + (1.0,)
    if "mix" in node:
        a, b, t = node["mix"]
        return mix(evaluate(a, R), evaluate(b, R), t)
    if "state" in node:
        b, o, op = node["state"]
        return state(evaluate(b, R), evaluate(o, R), op)
    if "alpha" in node:
        e, a = node["alpha"]
        return alpha(evaluate(e, R), a)
    raise ValueError(f"bad spec node: {node!r}")


# Buttons stay graphite with only a whisper of amber — a warm-tinted button base reads muddy
# against the cool chrome, and the amber identity belongs on the accents (tick, slider, overline).
_BTN = _mix("surfaceContainerHigh", "primary", 0.07)

MAP_IMGUI = {
    "Text": "onSurface",
    "TextDisabled": "onSurfaceVariant",
    "WindowBg": "surface",
    "ChildBg": _lit(0, 0, 0, 0),
    "PopupBg": "surfaceContainerHigh",
    "Border": "outlineVariant",
    "BorderShadow": _lit(0, 0, 0, 0),
    "FrameBg": "surfaceContainerHighest",
    "FrameBgHovered": _state("surfaceContainerHighest", "onSurface", 0.08),
    "FrameBgActive": _state("surfaceContainerHighest", "onSurface", 0.12),
    "TitleBg": "surfaceContainerLow",
    "TitleBgActive": "surfaceContainer",
    "TitleBgCollapsed": _ra("surfaceContainerLow", 0.85),
    "MenuBarBg": "surfaceContainer",
    "ScrollbarBg": _lit(0, 0, 0, 0),
    "ScrollbarGrab": "outlineVariant",
    "ScrollbarGrabHovered": "outline",
    "ScrollbarGrabActive": "primary",
    # ImGui swaps the checked box fill to FrameBgHovered (neutral) on hover (imgui_widgets
    # Checkbox), so the tick must read on BOTH the selected fill and the hover fill. A bright
    # tick on a dark container satisfies both; a dark tick on a bright fill fails on hover.
    "CheckMark": "primary",
    # Desaturated primaryContainer: the full container reads too hot as a solid checkbox fill,
    # but it must stay dark so the bright primary tick reads on it (and on the neutral hover fill).
    "CheckboxSelectedBg": _mix("primaryContainer", "surfaceContainerHighest", 0.5),
    "SliderGrab": "primary",
    "SliderGrabActive": _state("primary", "onSurface", 0.12),
    "Button": _BTN,
    "ButtonHovered": _state(_BTN, "onSurface", 0.08),
    "ButtonActive": _state(_BTN, "onSurface", 0.14),
    # Selection highlights (CollapsingHeader, MenuItem hover, Selectable, TreeNode): a subtle
    # secondary-tinted neutral, graduated by state-layer opacity. A solid secondaryContainer is
    # too strong as a large fill now that secondary carries real chroma (teal).
    "Header": _state("surfaceContainerHigh", "secondary", 0.16),
    "HeaderHovered": _state("surfaceContainerHigh", "secondary", 0.24),
    "HeaderActive": _state("surfaceContainerHigh", "secondary", 0.30),
    "Separator": "outlineVariant",
    "SeparatorHovered": "outline",
    "SeparatorActive": "primary",
    "ResizeGrip": _ra("primary", 0.20),
    "ResizeGripHovered": _ra("primary", 0.55),
    "ResizeGripActive": _ra("primary", 0.90),
    "InputTextCursor": "primary",
    "TabHovered": _state("surfaceContainerHighest", "onSurface", 0.08),
    "Tab": "surfaceContainer",
    "TabSelected": "surfaceContainerHighest",
    "TabSelectedOverline": "primary",
    "TabDimmed": "surfaceContainerLow",
    "TabDimmedSelected": "surfaceContainer",
    "TabDimmedSelectedOverline": _ra("primary", 0.55),
    "DockingPreview": _ra("primary", 0.45),
    "DockingEmptyBg": "surfaceContainerLowest",
    "PlotLines": "secondary",
    "PlotLinesHovered": "tertiary",
    "PlotHistogram": "primary",
    "PlotHistogramHovered": "tertiary",
    "TableHeaderBg": "surfaceContainer",
    "TableBorderStrong": "outline",
    "TableBorderLight": "outlineVariant",
    "TableRowBg": _lit(0, 0, 0, 0),
    "TableRowBgAlt": _ra("onSurface", 0.04),
    "TextLink": "primary",
    "TextSelectedBg": _ra("primary", 0.35),
    "TreeLines": "outlineVariant",
    "DragDropTarget": _ra("tertiary", 0.90),
    "DragDropTargetBg": _ra("tertiary", 0.15),
    "UnsavedMarker": "onSurfaceVariant",
    "NavCursor": "primary",
    "NavWindowingHighlight": _ra("onSurface", 0.70),
    "NavWindowingDimBg": _ra("inverseSurface", 0.20),
    "ModalWindowDimBg": _lit(0, 0, 0, 0.55),
}

MAP_APP = {
    "BandBarStripe": _ra("onSurface", 0.12),
    "BandBarText": "brightText",
    # Neutral fill for processing-region bands: axis-agnostic (assignment is shown via badge dots),
    # so it must read as a plain band on the recessed track in both schemes — a surface lifted
    # toward the text color, never an accent.
    "RegionBand": _mix("surface", "onSurface", 0.22),
    "Bookmark": _ra("secondary", 0.86),
    "BookmarkDot": "tertiary",
    "BookmarkDotHovered": _state("tertiary", "onSurface", 0.25),
    "PlayCursor": "error",
    "HudBg": _ra("surfaceContainerLowest", 0.80),
    "StripBg": "surfaceContainerLowest",
    "StripActiveBg": _mix("surface", "primary", 0.16),
    "StripHoverBg": _ra("onSurface", 0.05),
    "StripSeparator": _ra("outline", 0.80),
    "StripDivider": "outline",
    "LockIndicator": "tertiary",
    # Keep the top near surfaceContainerLowest (the bottom): a larger blend toward ...Low flips
    # direction between schemes (dark: lightens the top; light: darkens it), wrong in both. A small
    # blend + faint primary tint leaves just a subtle warm top-glow over the flat canvas.
    "CurveBgTop": _mix(_mix("surfaceContainerLowest", "surfaceContainerLow", 0.25), "primary", 0.05),
    "CurveBgBottom": "surfaceContainerLowest",
    "CurveHoverBg": _ra("onSurface", 0.015),
    # Opaque, muted foreground: visible against the recessed curve track without overpowering the
    # heat-colored curves that draw on top. onSurfaceVariant reads softer than full onSurface.
    "Waveform": "onSurfaceVariant",
    "GridLine": _ra("onSurfaceVariant", 0.30),
    "GridLineMid": _ra("onSurfaceVariant", 0.55),
    "SelectedLine": "primary",
    "DragPreview": "tertiary",
    "SelectionBox": "secondary",
    "SelectionBoxFill": _ra("secondary", 0.39),
    "OverlayLineMajor": _ra("onSurface", 0.47),
    "OverlayLineMinor": _ra("onSurface", 0.20),
    "ProcessingPanelBg": _ra("surface", 0.92),
    "SimViewBg": "surfaceContainerLowest",
    "SimDivider": _ra("outline", 0.80),
    "SimCrosshair": _ra("onSurfaceVariant", 0.40),
    "SimArcRef": "onSurfaceVariant",
    "SimArc": "tertiary",
    "TimelineCursorOuter": "onSurface",
    "TimelineCursorInner": "surface",
    # Fixed amber, identical in both schemes (not _dim("primary"), which diverges per scheme).
    "VideoTimelineFill": _lit(*_hex_to_srgb("#EEA853")),
    "TimelineVisibleRegionFill": _ra("onSurface", 0.24),
    "TimelineVisibleRegionBorder": _ra("onSurface", 0.47),
    "HeatmapBase": "surfaceContainerLowest",
    "UnsavedIndicator": "error",
    "ScriptSeekCursor": "onSurface",
    "ScriptPlayCursor": "error",
    "Success": "success",
    "Warning": "warning",
    "Error": "error",
}

MAP_IMNODES = {
    "NodeBackground": "surfaceContainerHigh",
    "NodeBackgroundHovered": _state("surfaceContainerHigh", "onSurface", 0.08),
    "NodeBackgroundSelected": _state("surfaceContainerHigh", "onSurface", 0.12),
    "NodeOutline": "outline",
    "TitleBar": "surfaceContainerHighest",
    "TitleBarHovered": _state("surfaceContainerHighest", "onSurface", 0.10),
    "TitleBarSelected": _state("surfaceContainerHighest", "onSurface", 0.16),
    "Link": "outline",
    "LinkHovered": "onSurfaceVariant",
    "LinkSelected": "primary",
    "Pin": "primary",
    "PinHovered": _state("primary", "onSurface", 0.15),
    "BoxSelector": _ra("primary", 0.25),
    "BoxSelectorOutline": _ra("primary", 0.70),
    "GridBackground": "surfaceContainerLowest",
    "GridLine": _ra("onSurfaceVariant", 0.18),
    "GridLinePrimary": _ra("onSurfaceVariant", 0.30),
    "MiniMapBackground": _ra("surfaceContainerLowest", 0.60),
    "MiniMapBackgroundHovered": _ra("surfaceContainerLowest", 0.80),
    "MiniMapOutline": _ra("outline", 0.50),
    "MiniMapOutlineHovered": "outline",
    "MiniMapNodeBackground": "surfaceContainerHighest",
    "MiniMapNodeBackgroundHovered": _state("surfaceContainerHighest", "onSurface", 0.10),
    "MiniMapNodeBackgroundSelected": "primary",
    "MiniMapNodeOutline": "outline",
    "MiniMapLink": "onSurfaceVariant",
    "MiniMapLinkSelected": "primary",
    "MiniMapCanvas": _ra("onSurface", 0.10),
    "MiniMapCanvasOutline": _ra("onSurface", 0.20),
}


# Geometry is scheme-independent, authored at 1x DPI. These give the app an identity
# distinct from stock ImGui (rounded corners, roomier spacing, hairline borders). The chrome
# carries a consistent corner radius — windows/popups/tabs at 6, frames/inputs/buttons/grabs at
# 5 (grab matches its frame track), scrollbars fully pilled at 9 — in step with the node editor's
# NodeCornerRounding (_APP_VARS / _NODE_VARS).
_IMGUI_VARS = {
    "WindowRounding": 6.0, "ChildRounding": 6.0, "FrameRounding": 5.0,
    "PopupRounding": 6.0, "ScrollbarRounding": 9.0, "GrabRounding": 5.0,
    "TabRounding": 6.0,
    "WindowBorderSize": 1.0, "FrameBorderSize": 0.0, "PopupBorderSize": 1.0,
    "ChildBorderSize": 1.0,
    "WindowPadding": [10.0, 9.0], "FramePadding": [9.0, 5.0],
    "ItemSpacing": [8.0, 6.0], "ItemInnerSpacing": [6.0, 5.0], "CellPadding": [6.0, 4.0],
    "ScrollbarSize": 13.0, "GrabMinSize": 11.0,
    "WindowTitleAlign": [0.0, 0.5], "TabBarOverlineSize": 2.0,
    "SeparatorTextBorderSize": 2.0,
}

_APP_VARS = {
    "ScriptSeekCursorWidth": 2.0, "ScriptPlayCursorWidth": 1.0,
    "GridLineMidWidth": 1.0, "OverlayLineMajorWidth": 1.0,
    "WaveformScale": 0.9,
    "SimGlobalOpacity": 0.75,
    "NodeGridSpacing": 24.0, "NodeCornerRounding": 5.0, "NodePadding": [10.0, 8.0],
    "NodeBorderThickness": 1.0, "NodeLinkThickness": 3.0, "NodePinRadius": 4.0,
}


# ---------------------------------------------------------------------------
# JSON assembly.
# ---------------------------------------------------------------------------

# Number of entries the "colors" JSON object must contain.
# = AppCol_COUNT - ImGuiCol_COUNT minus the 20 AxisDim* slots,
#   which are always derived in C++ (fillAxisDimColors) and never serialised.
_APP_COL_JSON_COUNT = 85


def _c(rgba):
    # "#RRGGBB" when opaque, "#RRGGBBAA" otherwise (RGBA order). Matches the C++ hex format
    # in src/Util/JsonImGui.h: round to 8-bit via int(v*255 + 0.5).
    q = lambda v: int(_clamp01(v) * 255 + 0.5)
    r, g, b, a = (q(x) for x in rgba)
    return "#%02X%02X%02X" % (r, g, b) if a >= 255 else "#%02X%02X%02X%02X" % (r, g, b, a)


def _v(val):
    return [round(val[0], 3), round(val[1], 3)] if isinstance(val, list) else round(val, 3)


def build_theme(name, dark, palettes):
    R = resolve_roles(palettes, _DARK_ROLES if dark else _LIGHT_ROLES)
    imgui = {k: evaluate(v, R) for k, v in MAP_IMGUI.items()}
    app = {k: evaluate(v, R) for k, v in MAP_APP.items()}
    app.update(embed_app(dark))
    assert len(app) == _APP_COL_JSON_COUNT, f"AppCol count drift: {len(app)} != {_APP_COL_JSON_COUNT}"
    nodes = {k: evaluate(v, R) for k, v in MAP_IMNODES.items()}
    return {
        "version": SCHEMA_VERSION,
        "name": name,
        "isDark": dark,
        "seed": None,  # filled by caller
        "imguiColors": {k: _c(v) for k, v in imgui.items()},
        "colors": {k: _c(v) for k, v in app.items()},
        "imguiVars": {k: _v(v) for k, v in _IMGUI_VARS.items()},
        "appVars": {k: _v(v) for k, v in _APP_VARS.items()},
        "nodes": {
            "colors": {k: _c(v) for k, v in nodes.items()},
        },
        "heatmapColors": [{"pos": round(p, 3), "color": _c(c)} for p, c in _HEATMAP_MARKS],
    }


def build_palette_json(seed_hex, palettes):
    tones = [0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 95, 99, 100]
    ramps = {name: {str(t): _srgb_to_hex(p.tone(t)) for t in tones} for name, p in palettes.items()}
    schemes = {}
    for label, dark in (("dark", True), ("light", False)):
        R = resolve_roles(palettes, _DARK_ROLES if dark else _LIGHT_ROLES)
        schemes[label] = {k: _srgb_to_hex(v) for k, v in R.items()}
    return {"seed": seed_hex, "palettes": ramps, "schemes": schemes}


# ---------------------------------------------------------------------------


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description="Generate ofs-ng base themes from a seed color.")
    ap.add_argument("--seed", default=DEFAULT_SEED, help="seed color hex, e.g. #EE935C")
    ap.add_argument("--out", type=Path, default=THEMES_DIR, help="output dir (default data/themes)")
    ap.add_argument("--dark-only", action="store_true", help="emit only dark.json")
    ap.add_argument("--light-only", action="store_true", help="emit only light.json")
    args = ap.parse_args()

    seed_rgb = _hex_to_srgb(args.seed)
    seed_hex = _srgb_to_hex(seed_rgb)
    palettes = build_palettes(seed_rgb)

    dark_theme = build_theme("Dark", True, palettes)
    light_theme = build_theme("Light", False, palettes)
    dark_theme["seed"] = seed_hex
    light_theme["seed"] = seed_hex

    written = []

    def emit(path, data):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(data if isinstance(data, str) else json.dumps(data, indent=2), encoding="utf-8")
        written.append(path)

    # dark/light go to data/themes (the source of truth). They are packed into assets.zip at build
    # time (cmake/PackAssets.cmake), so a regenerate takes effect on the next rebuild.
    if not args.light_only:
        emit(args.out / "dark.json", dark_theme)
    if not args.dark_only:
        emit(args.out / "light.json", light_theme)

    emit(args.out / "palette.json", build_palette_json(seed_hex, palettes))

    def rel(p):
        try:
            return p.relative_to(PROJECT_ROOT)
        except ValueError:
            return p

    print(f"seed {seed_hex}  ->")
    for p in written:
        print(f"  {rel(p)}")


if __name__ == "__main__":
    main()
