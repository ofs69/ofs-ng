#pragma once

// Typographic punctuation glyphs as UTF-8 literals, for labels and separators drawn in the regular
// text font (not the Lucide icon font — see Icons.h for those). These are not translatable text, so
// they stay literal; naming them keeps the raw byte escapes out of call sites and greppable here.
// Use via C string-literal concatenation, e.g. fmtScratch("A " GLYPH_MINUS " B").
// NOLINTBEGIN(modernize-macro-to-enum)

#define GLYPH_MIDDLE_DOT "\xc2\xb7"      // · interpunct (U+00B7) — inline separator
#define GLYPH_DEGREE "\xc2\xb0"          // ° (U+00B0)
#define GLYPH_EN_DASH "\xe2\x80\x93"     // – (U+2013) — range separator ("a – b")
#define GLYPH_EM_DASH "\xe2\x80\x94"     // — (U+2014)
#define GLYPH_ELLIPSIS "\xe2\x80\xa6"    // … (U+2026)
#define GLYPH_ARROW_RIGHT "\xe2\x86\x92" // → (U+2192)
#define GLYPH_MINUS "\xe2\x88\x92"       // − (U+2212) minus sign (not the ASCII hyphen-minus)
#define GLYPH_MULTIPLY "\xc3\x97"        // × (U+00D7)
#define GLYPH_DIVIDE "\xc3\xb7"          // ÷ (U+00F7)

// NOLINTEND(modernize-macro-to-enum)
