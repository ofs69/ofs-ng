#!/usr/bin/env python3
"""Regenerate src/UI/Icons.h from data/fonts/lucide.ttf.

The file has two parts:
  1. A hand-curated alias block (CURATED below) — short app-specific names like
     ICON_SEEK, ICON_BAKE, ICON_APP that the codebase uses. Edit these by hand.
  2. An auto-generated block with one `#define ICON_<NAME>` per glyph in the
     font's Private Use Area (>= U+E000), named after the canonical Lucide icon.
     A canonical define is skipped when its macro name collides with a curated
     alias, so the alias wins (no /WX redefinition error).

Re-run after updating the .ttf — Lucide's PUA mapping shifts between versions.
"""
import re
import sys
from pathlib import Path
from fontTools.ttLib import TTFont

ROOT = Path(__file__).resolve().parent.parent
FONT = ROOT / "data" / "fonts" / "lucide.ttf"
OUT = ROOT / "src" / "UI" / "Icons.h"

HEADER = """#pragma once

// Lucide icon codepoints as UTF-8 literals (font: data/fonts/lucide.ttf).
// Re-generate with tools/gen_icons_header.py after updating the .ttf — Lucide's
// PUA mapping shifts between font versions.
// NOLINTBEGIN(modernize-macro-to-enum)
"""

# Hand-curated short aliases. Kept verbatim; the generated block below will not
# redefine any of these macro names.
CURATED = """
// ── Curated aliases (hand-maintained) ───────────────────────────────────────
// App / branding
#define ICON_APP "\\xee\\x95\\x9b" // lucide: audio-waveform (U+E55B)

// Media transport
#define ICON_STEP_BACKWARD "\\xee\\x85\\x9f" // lucide: skip-back (U+E15F)
#define ICON_BACKWARD "\\xee\\x85\\x87"      // lucide: rewind (U+E147)
#define ICON_PLAY "\\xee\\x84\\xbc"          // lucide: play (U+E13C)
#define ICON_PAUSE "\\xee\\x84\\xae"         // lucide: pause (U+E12E)
#define ICON_FORWARD "\\xee\\x82\\xbd"       // lucide: fast-forward (U+E0BD)
#define ICON_STEP_FORWARD "\\xee\\x85\\xa0"  // lucide: skip-forward (U+E160)
#define ICON_VOLUME_OFF "\\xee\\x86\\xac"    // lucide: volume-x (U+E1AC)
#define ICON_VOLUME_UP "\\xee\\x86\\xab"     // lucide: volume-2 (U+E1AB)

// Actions / editing
#define ICON_TRASH "\\xee\\x86\\x8e"   // lucide: trash-2 (U+E18E)
#define ICON_EDIT "\\xee\\x87\\xb9"    // lucide: pencil (U+E1F9)
#define ICON_PLUS "\\xee\\x84\\xbd"    // lucide: plus (U+E13D)
#define ICON_SAVE "\\xee\\x85\\x8d"    // lucide: save (U+E14D)
#define ICON_EXPORT "\\xee\\x82\\xb2"  // lucide: download (U+E0B2)
#define ICON_RESET "\\xee\\x85\\x88"   // lucide: rotate-ccw (U+E148)
#define ICON_REFRESH "\\xee\\x85\\x85" // lucide: refresh-cw (U+E145)
#define ICON_BAKE "\\xee\\x86\\xb4"    // lucide: zap (U+E1B4)
#define ICON_COPY "\\xee\\x82\\x9e"    // lucide: copy (U+E09E)

// Files / navigation
#define ICON_FOLDER "\\xee\\x83\\x97"      // lucide: folder (U+E0D7)
#define ICON_FOLDER_OPEN "\\xee\\x89\\x87" // lucide: folder-open (U+E247)
#define ICON_BOOKMARK "\\xee\\x81\\xa0"    // lucide: bookmark (U+E060)
#define ICON_CHAPTER "\\xee\\x81\\x9f"     // lucide: book-open (U+E05F)
#define ICON_SEEK "\\xee\\x84\\x91"        // lucide: map-pin (U+E111)
#define ICON_RECENTER "\\xee\\x82\\xac"    // lucide: crosshair (U+E0AC)

// Categories (command palette groups, panels)
#define ICON_SETTINGS "\\xee\\x85\\x94" // lucide: settings (U+E154)
#define ICON_PLUGIN "\\xee\\x8a\\x9c"   // lucide: puzzle (U+E29C)
#define ICON_MOVE "\\xee\\x84\\xa1"     // lucide: move (U+E121)
#define ICON_NAV "\\xee\\x82\\x9b"      // lucide: compass (U+E09B)
#define ICON_LIST "\\xee\\x84\\x86"     // lucide: list (U+E106)
#define ICON_EYE "\\xee\\x82\\xba"      // lucide: eye (U+E0BA)
#define ICON_EYE_OFF "\\xee\\x82\\xbb"  // lucide: eye-off (U+E0BB)

// State
#define ICON_LINK "\\xee\\x84\\x82"      // lucide: link (U+E102)
#define ICON_UNLINK "\\xee\\x86\\x9c"    // lucide: unlink (U+E19C)
#define ICON_LOCK "\\xee\\x84\\x8b"      // lucide: lock (U+E10B)
#define ICON_LOCK_OPEN "\\xee\\x84\\x8c" // lucide: lock-open (U+E10C)

// Simulator 3D axes (translation = directional move, rotation = distinct rotate glyphs)
#define ICON_MOVE_VERTICAL "\\xee\\x87\\x87"   // lucide: move-vertical (U+E1C7)
#define ICON_MOVE_HORIZONTAL "\\xee\\x87\\x86" // lucide: move-horizontal (U+E1C6)
#define ICON_MOVE_3D "\\xee\\x8b\\xa5"         // lucide: move-3-d (U+E2E5)
#define ICON_ROTATE_3D "\\xee\\x8b\\xaa"       // lucide: rotate-3-d (U+E2EA)
#define ICON_AXIS_3D "\\xee\\x8b\\xbe"         // lucide: axis-3-d (U+E2FE)
#define ICON_ORBIT "\\xee\\x8f\\xa7"           // lucide: orbit (U+E3E7)
"""

FOOTER = "// NOLINTEND(modernize-macro-to-enum)\n"


def utf8_literal(cp: int) -> str:
    return "".join(f"\\x{b:02x}" for b in chr(cp).encode("utf-8"))


def macro_name(name: str) -> str:
    return "ICON_" + name.upper().replace("-", "_")


def main() -> int:
    curated_macros = set(re.findall(r"#define (ICON_\w+)", CURATED))

    font = TTFont(str(FONT))
    cmap = font.getBestCmap()
    icons = sorted(
        ((name, cp) for cp, name in cmap.items() if cp >= 0xE000),
        key=lambda x: x[0],
    )

    seen = set(curated_macros)
    lines = []
    for name, cp in icons:
        macro = macro_name(name)
        if macro in seen:
            continue  # curated alias or earlier glyph alias wins
        seen.add(macro)
        lines.append(f'#define {macro} "{utf8_literal(cp)}" // lucide: {name} (U+{cp:04X})')

    body = (
        HEADER
        + CURATED
        + "\n// ── Generated from the font cmap (canonical Lucide names) ───────────────────\n"
        + "\n".join(lines)
        + "\n"
        + FOOTER
    )
    OUT.write_text(body, encoding="utf-8")
    print(f"wrote {len(lines)} generated + {len(curated_macros)} curated icons to {OUT}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
