#include "Localization/Translator.h"
#include "Core/StandardAxis.h"
#include "Localization/AxisNames.h"
#include "Localization/TrKey.h"
#include "Util/FrameAllocator.h"
#include "Util/Log.h"
#include "Util/PathUtil.h"

#include "Util/FileUtil.h"
#include "Util/Resources.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <set>
#include <sstream>
#include <system_error>
#include <toml++/toml.hpp>
#include <utility>

namespace ofs::loc {

namespace {

constexpr int kMaxPlaceholders = 64; // RequiredMask is a uint64_t bitmask

// Parse a TOML file given its path. We read the bytes ourselves rather than calling
// toml::parse_file, whose narrow overload decodes the path as ANSI on Windows — this keeps
// non-ASCII paths working on every platform with a single code path. Throws toml::parse_error
// on malformed input, exactly like parse_file.
toml::table parseTomlFile(const std::filesystem::path &path) {
    return toml::parse(ofs::util::readFile(path).value_or(""), ofs::util::toUtf8(path));
}

bool isWhitespaceOnly(std::string_view s) {
    return std::ranges::all_of(s, [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; });
}

// Read the declared ISO 639 code from a parsed lang file's [_meta] iso639 field; empty if absent.
// The code is read ONLY from the file content — never derived from the filename/language id.
std::string readLanguageCode(const toml::table &root) {
    if (const auto *meta = root["_meta"].as_table())
        if (auto v = (*meta)["iso639"].value<std::string>(); v && !isWhitespaceOnly(*v))
            return *v;
    return {};
}

// Bitmask of explicit {N} placeholder indices in `text`. Honors {{ and }} escapes. Malformed or
// out-of-range braces are ignored — the resulting mask simply won't match RequiredMask, so the
// entry falls back to its default. Never throws.
std::uint64_t placeholderMask(std::string_view text) {
    std::uint64_t mask = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '}') {
            if (i + 1 < text.size() && text[i + 1] == '}')
                ++i;
            continue;
        }
        if (text[i] != '{')
            continue;
        if (i + 1 < text.size() && text[i + 1] == '{') {
            ++i;
            continue;
        }
        std::size_t j = i + 1;
        std::uint64_t idx = 0;
        bool hasDigits = false;
        while (j < text.size() && (std::isdigit(static_cast<unsigned char>(text[j])) != 0)) {
            idx = idx * 10 + static_cast<std::uint64_t>(text[j] - '0');
            hasDigits = true;
            ++j;
        }
        if (hasDigits && j < text.size() && text[j] == '}' && idx < kMaxPlaceholders) {
            mask |= (std::uint64_t{1} << idx);
            i = j;
        }
    }
    return mask;
}

void writeRootAsToml(std::ostream &out, const toml::table &root) {
    auto writeTable = [&out](std::string_view name, const toml::table &entry) {
        out << "\n[" << name << "]\n";
        for (auto &&[field, val] : entry) {
            if (const auto *arr = val.as_array(); arr && field.str() == "placeholders") {
                out << "placeholders = [\n";
                for (const auto &elem : *arr)
                    out << "    " << toml::toml_formatter{elem} << ",\n";
                out << "]\n";
            } else {
                out << field.str() << " = " << toml::toml_formatter{val} << "\n";
            }
        }
    };
    // Emit [_meta] first so a reader (and the language picker) sees the file's metadata before the
    // string entries; toml::table iteration order otherwise sorts "_meta" in amongst the keys.
    if (const auto *meta = root["_meta"].as_table())
        writeTable("_meta", *meta);
    for (auto &&[key, node] : root) {
        if (key.str() == "_meta")
            continue;
        const auto *entry = node.as_table();
        if (!entry)
            continue;
        writeTable(key.str(), *entry);
    }
}

toml::table buildEntry(std::uint32_t idx, std::string_view translation) {
    toml::table entry;
    entry.insert("description", gen::Description[idx]);
    if (gen::PlaceholderCount[idx] > 0) {
        toml::array phs;
        for (std::uint32_t p = 0; p < gen::PlaceholderCount[idx]; ++p)
            phs.push_back(gen::Placeholders[idx][p]);
        entry.insert("placeholders", std::move(phs));
    }
    entry.insert("english", gen::Default[idx]);
    entry.insert("translation", translation);
    return entry;
}

// Build a full strings-shaped catalog (optional [_meta].iso639 + one annotated entry per key, each
// entry's translation supplied by `perKey(idx)`), prepend `header`, and write it to `path`. The shared
// body of exportCatalog and refreshTranslation, which differ only in the per-key source and the header.
// `forceMeta` emits the [_meta] table even for an empty code (export always declares one); otherwise it
// is emitted only when non-empty. Logs and returns false on a write failure; the caller logs success.
template <typename PerKeyFn>
bool writeCatalogToFile(const std::filesystem::path &path, std::string_view iso639, bool forceMeta, PerKeyFn &&perKey,
                        std::string_view header) {
    toml::table root;
    if (forceMeta || !iso639.empty()) {
        toml::table meta;
        meta.insert("iso639", std::string(iso639));
        root.insert("_meta", std::move(meta));
    }
    for (std::uint32_t idx = 1; idx < gen::Count; ++idx)
        root.insert(gen::KeyName[idx], buildEntry(idx, perKey(idx)));

    // The export defaults under <pref>/lang, which need not exist yet on a fresh install.
    std::error_code ec;
    if (path.has_parent_path())
        std::filesystem::create_directories(path.parent_path(), ec);

    std::ostringstream out;
    out << header;
    writeRootAsToml(out, root);
    if (!ofs::util::writeFileAtomic(path, out.str())) {
        OFS_CORE_ERROR("Localization: cannot write catalog to '{}'", ofs::util::toUtf8(path));
        return false;
    }
    return true;
}

} // namespace

Translator &Translator::instance() {
    static Translator t;
    return t;
}

Translator::Translator() {
    loadDefaults();
}

void Translator::loadDefaults() {
    for (std::uint32_t i = 0; i < gen::Count; ++i)
        translation[i] = gen::Default[i];
    stringData_.clear();
    activeLanguageId_ = "en";
    activeLanguageCode_ = "en";
    activeFilePath_.clear();
}

bool Translator::load(std::string_view languageId) {
    if (languageId.empty() || languageId == "en") {
        loadDefaults();
        return true;
    }

    // A user override on disk (<pref>/lang/<id>.toml) wins over the shipped translation so users can
    // patch one; the shipped catalog itself lives in the assets archive (lang/<id>.toml).
    std::error_code ec;
    const std::filesystem::path userPath =
        ofs::util::getPrefPath() / "lang" / ofs::util::fromUtf8(fmt::format("{}.toml", languageId));
    const bool fromUser = std::filesystem::exists(userPath, ec);
    const std::string archiveName = fmt::format("lang/{}.toml", languageId);
    const std::string label = fromUser ? ofs::util::toUtf8(userPath) : archiveName;

    toml::table root;
    try {
        if (fromUser) {
            root = parseTomlFile(userPath);
        } else {
            auto text = ofs::res::readText(archiveName);
            if (!text) {
                OFS_CORE_WARN("Localization: no file found for language '{}'", languageId);
                return false;
            }
            root = toml::parse(*text, label);
        }
    } catch (const toml::parse_error &e) {
        OFS_CORE_ERROR("Localization: failed to parse '{}': {}", label, e.description());
        return false;
    }

    // Collect accepted (index, translation) pairs without touching member state yet.
    std::vector<std::pair<std::uint32_t, std::string>> accepted;
    for (auto &&[key, node] : root) {
        const std::uint32_t idx = gen::keyToTr(key.str());
        if (idx == 0)
            continue; // unknown key — forward-compat, ignore
        const auto *sub = node.as_table();
        if (sub == nullptr)
            continue;
        auto value = (*sub)["translation"].value<std::string>();
        if (!value || isWhitespaceOnly(*value))
            continue; // missing/empty -> keep English default
        if (placeholderMask(*value) != gen::RequiredMask[idx]) {
            OFS_CORE_WARN("Localization: placeholder mismatch for key '{}' in '{}' — using English default",
                          gen::KeyName[idx], label);
            continue;
        }
        accepted.emplace_back(idx, std::move(*value));
    }

    // Build the contiguous blob, then commit. From here on nothing throws.
    std::size_t total = 0;
    for (const auto &[idx, str] : accepted)
        total += str.size() + 1;

    std::vector<char> blob;
    blob.reserve(total);
    std::vector<std::pair<std::uint32_t, std::size_t>> offsets;
    offsets.reserve(accepted.size());
    for (const auto &[idx, str] : accepted) {
        offsets.emplace_back(idx, blob.size());
        blob.insert(blob.end(), str.begin(), str.end());
        blob.push_back('\0');
    }

    stringData_ = std::move(blob);
    for (std::uint32_t i = 0; i < gen::Count; ++i)
        translation[i] = gen::Default[i];
    for (const auto &[idx, off] : offsets)
        translation[idx] = stringData_.data() + off;

    activeLanguageId_ = std::string(languageId);
    // ISO 639 code the file declares in [_meta].iso639 — read from the file only, never from the
    // filename. This is the code plugins receive (via getActiveLanguage at load). A file that omits it
    // (e.g. a user-exported catalog) yields "en", so plugins show their neutral catalog.
    activeLanguageCode_ = readLanguageCode(root);
    if (activeLanguageCode_.empty())
        activeLanguageCode_ = "en";
    // Only disk-backed (user-override) languages support live reload; archived ones have no mtime to poll.
    activeFilePath_ = fromUser ? userPath : std::filesystem::path{};
    if (fromUser)
        lastWriteTime_ = std::filesystem::last_write_time(userPath, ec);

    OFS_CORE_INFO("Localization: loaded '{}' ({} translated keys)", languageId, accepted.size());
    return true;
}

std::vector<std::string> Translator::available() const {
    std::set<std::string> ids;
    ids.insert("en");
    // Shipped translations live in the assets archive (lang/<id>.toml).
    for (const auto &name : ofs::res::list("lang/")) {
        const std::filesystem::path p = ofs::util::fromUtf8(name);
        if (p.extension() == ".toml")
            ids.insert(ofs::util::toUtf8(p.stem()));
    }
    // User overrides on disk join the list.
    std::error_code ec;
    const std::filesystem::path userDir = ofs::util::getPrefPath() / "lang";
    for (std::filesystem::directory_iterator it(userDir, ec), end; it != end; it.increment(ec)) {
        if (ec)
            break;
        const auto &p = it->path();
        if (p.extension() == ".toml")
            ids.insert(ofs::util::toUtf8(p.stem()));
    }
    return {ids.begin(), ids.end()};
}

bool Translator::exportCatalog(const std::filesystem::path &path) const {
    // Carry the active language's ISO 639 code into the export so the file declares one (a new language
    // edits it to its own code). Without it the catalog has no code and plugins fall back to English.
    // Pre-fill each entry's translation with the active language's text so the user exports the
    // currently-selected language and patches it from there. load() leaves untranslated keys pointing at
    // the baked-in default (same pointer), so a pointer match means "not translated" → emit an empty
    // translation (the English source still shows in the `english` field).
    const bool ok = writeCatalogToFile(
        path, activeLanguageCode_, /*forceMeta=*/true,
        [this](std::uint32_t idx) -> std::string_view {
            const char *tr = translation[idx];
            return (tr != nullptr && tr != gen::Default[idx]) ? std::string_view(tr) : std::string_view{};
        },
        "# Translation catalog — ofs-ng localization.\n"
        "#\n"
        "# One [Key] table per UI string. Fill in the 'translation' field for each entry.\n"
        "# Leave 'translation' empty to fall back to the English default at runtime.\n"
        "#\n"
        "# [_meta].iso639 is this catalog's ISO 639 language code (e.g. \"ja\"); set it to the\n"
        "# language you are translating into. ofs-ng hands it to plugins so they localize to match.\n"
        "#\n"
        "# Fields:\n"
        "#   description  — context: what the string is / where it appears.\n"
        "#   placeholders — docs for {0}, {1}, … placeholders (omitted when none).\n"
        "#   english      — the source string (do not modify).\n"
        "#   translation  — your translated text. Must use the same {N} placeholders.\n");
    if (ok)
        OFS_CORE_INFO("Localization: exported catalog to '{}'", ofs::util::toUtf8(path));
    return ok;
}

bool Translator::refreshTranslation(const std::filesystem::path &path) const {
    std::map<std::string, std::string> existing;
    std::string iso639; // carried over from the file's [_meta] so a refresh never drops its language code
    {
        toml::table parsed;
        try {
            parsed = parseTomlFile(path);
        } catch (const toml::parse_error &e) {
            OFS_CORE_ERROR("Localization: cannot refresh '{}': {}", ofs::util::toUtf8(path), e.description());
            return false;
        }
        iso639 = readLanguageCode(parsed);
        for (auto &&[key, node] : parsed) {
            const auto *sub = node.as_table();
            if (sub == nullptr)
                continue;
            if (auto value = (*sub)["translation"].value<std::string>())
                existing.emplace(std::string(key.str()), std::move(*value));
        }
    }

    const bool ok = writeCatalogToFile(
        path, iso639, /*forceMeta=*/false,
        [&existing](std::uint32_t idx) -> std::string_view {
            auto it = existing.find(gen::KeyName[idx]);
            return (it != existing.end()) ? std::string_view(it->second) : std::string_view{};
        },
        "# Translation catalog refreshed by ofs-ng against the current source.\n");
    if (ok)
        OFS_CORE_INFO("Localization: refreshed translation '{}'", ofs::util::toUtf8(path));
    return ok;
}

void Translator::pollReload() {
    if (activeFilePath_.empty())
        return;
    std::error_code ec;
    auto t = std::filesystem::last_write_time(activeFilePath_, ec);
    if (ec || t == lastWriteTime_)
        return;
    OFS_CORE_INFO("Localization: '{}' changed on disk — reloading", ofs::util::toUtf8(activeFilePath_));
    load(activeLanguageId_);
}

// ── Free helpers backing TrKey (declared in TrKey.h) ─────────────────────────────────────────

const char *trLookup(std::uint32_t index) {
    if (index >= gen::Count)
        return "";
    const char *p = Translator::instance().translation[index];
    return p != nullptr ? p : "";
}

const char *trId(std::uint32_t index, const char *stableId) {
    return fmtScratch("{}###{}", trLookup(index), stableId);
}

const char *trIcon(std::uint32_t index, const char *glyph) {
    return fmtScratch("{} {}", glyph, trLookup(index));
}

const char *trIconId(std::uint32_t index, const char *glyph, const char *stableId) {
    return fmtScratch("{} {}###{}", glyph, trLookup(index), stableId);
}

const char *localizedAxisName(StandardAxis axis) {
    const std::string_view code = standardAxisShortName(axis);
    const TrKey *desc = nullptr;
    switch (axis) {
    case StandardAxis::L0:
        desc = &Str::AxisDescL0;
        break;
    case StandardAxis::L1:
        desc = &Str::AxisDescL1;
        break;
    case StandardAxis::L2:
        desc = &Str::AxisDescL2;
        break;
    case StandardAxis::R0:
        desc = &Str::AxisDescR0;
        break;
    case StandardAxis::R1:
        desc = &Str::AxisDescR1;
        break;
    case StandardAxis::R2:
        desc = &Str::AxisDescR2;
        break;
    case StandardAxis::V0:
        desc = &Str::AxisDescV0;
        break;
    case StandardAxis::V1:
        desc = &Str::AxisDescV1;
        break;
    case StandardAxis::A0:
        desc = &Str::AxisDescA0;
        break;
    case StandardAxis::A1:
        desc = &Str::AxisDescA1;
        break;
    default:
        break; // scratch axes (S0–S9): code only, no descriptor
    }
    return desc != nullptr ? fmtScratch("{} ({})", code, desc->c_str()) : fmtScratch("{}", code);
}

const char *trFormat(std::uint32_t index, fmt::format_args args) {
    const char *fmtStr = trLookup(index);
    auto &fa = ofs::FrameAllocator::instance();
    char *out = fa.currentPos();
    const std::size_t rem = fa.remaining();
    if (rem == 0)
        return "";
    const std::size_t maxWrite = rem - 1;
    try {
        // One transient heap string from vformat, then copied into the frame arena. Formatted
        // strings are far rarer than plain labels (which take the zero-alloc trLookup path), and
        // load-time validation already guarantees a well-formed format string here.
        std::string formatted = fmt::vformat(fmt::string_view(fmtStr), args);
        const std::size_t n = std::min(formatted.size(), maxWrite);
        std::memcpy(out, formatted.data(), n);
        out[n] = '\0';
        fa.advance(n + 1);
        return out;
    } catch (const std::exception &e) {
        // Load-time validation should make this unreachable; warn once if it ever fires so a malformed
        // catalog entry surfaces in the log instead of silently rendering its raw format string.
        static bool warned = false;
        if (!warned) {
            warned = true;
            OFS_CORE_WARN("Localization: bad format string '{}' ({})", fmtStr, e.what());
        }
        return fmtStr; // never crash on a bad format — show the unformatted string
    }
}

} // namespace ofs::loc
