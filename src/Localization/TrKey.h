#pragma once

// TrKey / TrString — the macro-free call-site surface for localized strings.
//
// The build-time generator emits one `inline constexpr TrKey` per catalog key into a global
// `Str::` namespace (see StringsGenerated.h). UI code uses them directly:
//
//     ImGui::Button(Str::Ok);                       // implicit -> const char*
//     ImGui::CollapsingHeader(Str::Settings.id("settings"));   // "label###settings"
//     ImGui::TextUnformatted(Str::StatusActionsMoved.fmt(count, ms));
//
// TrKey only stores a uint32_t index. Resolution goes through the free helpers below, which are
// defined in Translator.cpp — this header has no dependency on the Translator type, breaking the
// otherwise-circular include (Translator.h includes the generated header, which includes this).

#include <cstdint>
#include <cstring>
#include <spdlog/fmt/fmt.h>
#include <string>
#include <string_view>
#include <variant>

namespace ofs::loc {

// Defined in Translator.cpp. All run on the main thread and return frame-arena / static storage.
const char *trLookup(uint32_t index);                        // -> Translator Translation[index]
std::string_view trLookupSv(uint32_t index);                 // -> same text, length-carrying (no strlen)
const char *trId(uint32_t index, const char *stableId);      // -> "visible label###stableId" (frame arena)
const char *trFormat(uint32_t index, fmt::format_args args); // -> validated, formatted (frame arena)
const char *trIcon(uint32_t index, const char *glyph);       // -> "<glyph> label" (frame arena)
const char *trIconId(uint32_t index, const char *glyph, const char *stableId); // -> "<glyph> label###id"

} // namespace ofs::loc

// A localized string key. Trivial, constexpr-constructible; lives in the global namespace so call
// sites stay terse (mirroring the global `fmtScratch`).
struct TrKey {
    uint32_t index;

    constexpr explicit TrKey(uint32_t i) : index(i) {}

    // The one piece of implicit magic, deliberately confined to this purpose-built type so plain
    // labels read as plain labels. NOLINT is targeted, not a relaxation elsewhere.
    // NOLINTNEXTLINE(google-explicit-constructor,hicpp-explicit-conversions)
    operator const char *() const { return ofs::loc::trLookup(index); }

    // Compose an ImGui id: "<translated label>###<stableId>". The ###id is appended at the call
    // site and never stored in the catalog, so translating the label never resets widget state.
    const char *id(const char *stableId) const { return ofs::loc::trId(index, stableId); }

    // Compose "<icon> <translated label>" into the frame arena — a button/menu caption with a leading
    // glyph. The icon is not translatable and stays literal. iconId() additionally appends a stable
    // language-independent widget id (see id()): "<icon> <translated label>###<stableId>".
    const char *icon(const char *glyph) const { return ofs::loc::trIcon(index, glyph); }
    const char *iconId(const char *glyph, const char *stableId) const {
        return ofs::loc::trIconId(index, glyph, stableId);
    }

    // Resolve to the current translation as a plain C string. Identical result to the implicit
    // const char* conversion, but spelled out — needed where the implicit conversion can't fire:
    // a printf-style vararg (ImGui::Text("%s", k.c_str())), an fmt argument, or a std::string
    // parameter (an implicit TrKey->const char*->std::string would be two user conversions).
    const char *c_str() const { return ofs::loc::trLookup(index); }

    // The active translation as a length-carrying view. Prefer this over c_str() when handing the label
    // to fmt/fmtScratch ("{}", k.sv()) — fmt uses the known size instead of a strlen, and it lets a
    // TrKey field be formatted directly (fmt has no formatter for TrKey itself). Valid until the next
    // language load; for an immediate format-and-discard that is always safe.
    std::string_view sv() const { return ofs::loc::trLookupSv(index); }

    // Validated runtime formatting. The translation is fetched by index and used as the format
    // string; args fill {0}, {1}, … The result lives in the frame arena (valid until the next
    // FrameAllocator::reset()). `args` are named lvalues here, as fmt::make_format_args requires.
    template <class... Args> const char *fmt(Args &&...args) const {
        // ::fmt:: — the method is named fmt, so the unqualified namespace would resolve to it.
        return ofs::loc::trFormat(index, ::fmt::make_format_args(args...));
    }
};

// Carries a value that is either a localized key or a user-supplied literal (e.g. a user-named
// item that must never be auto-translated). c_str() resolves whichever it holds.
struct TrString {
    std::variant<TrKey, std::string> value;

    TrString(TrKey key) : value(key) {} // NOLINT(google-explicit-constructor,hicpp-explicit-conversions)
    TrString(std::string literal)
        : value(std::move(literal)) {} // NOLINT(google-explicit-constructor,hicpp-explicit-conversions)
    // A bare string literal is a user-supplied literal too. Without this, `const char*` → TrString would
    // need two user conversions (→ std::string → TrString) and fail to bind implicitly at call sites.
    TrString(const char *literal)
        : value(std::string(literal)) {} // NOLINT(google-explicit-constructor,hicpp-explicit-conversions)

    const char *c_str() const {
        if (const auto *key = std::get_if<TrKey>(&value))
            return ofs::loc::trLookup(key->index);
        return std::get<std::string>(value).c_str();
    }

    // Compare against a raw string by resolved value (the literal it holds, or the active translation of
    // its key). Lets call sites and tests check display text without spelling out .c_str() each time.
    bool operator==(const char *rhs) const { return std::strcmp(c_str(), rhs) == 0; }
    bool operator==(const std::string &rhs) const { return rhs == c_str(); }
};
