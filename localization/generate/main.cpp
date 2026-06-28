// localization_gen — build-time code generator for ofs-ng localization.
//
// Usage: localization_gen <strings.toml> <out_header.h> <out_source.cpp>
//
// Reads the English source catalog (one [Key] table per string) and emits the committed
// StringsGenerated.{h,cpp}: the `Tr` index enum, the `Str::` constexpr call-site constants,
// the baked-in English defaults, reference metadata (description / placeholder docs), a
// key->Tr lookup, and per-key placeholder bitmasks used for load-time validation.
//
// Fails loudly (non-zero exit + message on stderr) on a duplicate key, an empty key, a key that
// is not a valid C++ identifier, or a malformed/mismatched placeholder list — so a broken catalog
// breaks the build rather than shipping.

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <toml++/toml.hpp>

namespace {

struct Entry {
    std::string key;
    std::string english;
    std::string description;
    std::vector<std::string> placeholders;
    std::uint64_t requiredMask = 0; // bit i set => placeholder {i} occurs in `english`
    int distinctCount = 0;          // number of distinct placeholder indices
};

[[noreturn]] void fail(const std::string &msg) {
    std::cerr << "localization_gen: " << msg << "\n";
    std::exit(1);
}

bool isValidIdentifier(std::string_view s) {
    if (s.empty())
        return false;
    if (!(std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_'))
        return false;
    return std::all_of(s.begin() + 1, s.end(),
                       [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; });
}

// Parse fmt-style placeholders in `text`. Honors {{ and }} escapes. Requires explicit indices
// ({0}, {1}, …): a bare {} is rejected so translators can always reorder unambiguously.
// Returns the bitmask of indices used; reports the distinct count via `outDistinct`.
std::uint64_t parsePlaceholders(const std::string &key, const std::string &text, int &outDistinct) {
    std::uint64_t mask = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (c == '}') {
            if (i + 1 < text.size() && text[i + 1] == '}') {
                ++i; // "}}" literal
                continue;
            }
            continue; // stray '}' — ignore
        }
        if (c != '{')
            continue;
        if (i + 1 < text.size() && text[i + 1] == '{') {
            ++i; // "{{" literal
            continue;
        }
        // Expect digits then '}'.
        std::size_t j = i + 1;
        std::string digits;
        while (j < text.size() && std::isdigit(static_cast<unsigned char>(text[j]))) {
            digits.push_back(text[j]);
            ++j;
        }
        if (digits.empty() || j >= text.size() || text[j] != '}')
            fail("key '" + key +
                 "': placeholders must use explicit indices like {0}, {1} (got bare or malformed '{' in \"" + text +
                 "\")");
        unsigned long idx = std::stoul(digits);
        if (idx >= 64)
            fail("key '" + key + "': placeholder index {" + digits + "} exceeds the supported maximum (63)");
        mask |= (std::uint64_t{1} << idx);
        i = j; // resume after '}'
    }
    outDistinct = 0;
    for (int b = 0; b < 64; ++b)
        if (mask & (std::uint64_t{1} << b))
            ++outDistinct;
    return mask;
}

// Lenient placeholder mask, mirroring the runtime loader (ofs::loc::placeholderMask): malformed or
// out-of-range braces are ignored rather than rejected. Used in --validate mode so a translation is
// judged by the exact same rule the app applies when it loads — "would this be accepted at runtime?".
std::uint64_t lenientMask(std::string_view text) {
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
        if (hasDigits && j < text.size() && text[j] == '}' && idx < 64) {
            mask |= (std::uint64_t{1} << idx);
            i = j;
        }
    }
    return mask;
}

// Escape a string for embedding in a C++ "..." literal.
std::string cppEscape(const std::string &s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            out += c;
            break;
        }
    }
    return out;
}

std::string readString(const toml::table &tbl, const std::string &key, const char *field, bool required) {
    auto node = tbl[field];
    if (!node) {
        if (required)
            fail("key '" + key + "': missing required field '" + std::string(field) + "'");
        return {};
    }
    auto val = node.value<std::string>();
    if (!val)
        fail("key '" + key + "': field '" + std::string(field) + "' must be a string");
    return *val;
}

// Parse + validate the English source catalog into a sorted Entry list. Fails the build (fail())
// on a malformed file, duplicate/empty/invalid key, missing 'english', or a placeholder-doc count
// that disagrees with the 'english' text. Shared by both the generate and --validate modes.
std::vector<Entry> loadSource(const std::string &inPath) {
    toml::table root;
    try {
        root = toml::parse_file(inPath);
    } catch (const toml::parse_error &e) {
        fail("failed to parse " + inPath + ": " + std::string(e.description()));
    }

    std::vector<Entry> entries;
    std::set<std::string> seen;
    for (auto &&[key, node] : root) {
        std::string keyStr(key.str());
        if (!isValidIdentifier(keyStr))
            fail("key '" + keyStr + "' is empty or not a valid C++ identifier");
        if (!seen.insert(keyStr).second)
            fail("duplicate key '" + keyStr + "'");

        const auto *sub = node.as_table();
        if (sub == nullptr)
            fail("key '" + keyStr + "' must be a table ([" + keyStr + "])");

        Entry e;
        e.key = keyStr;
        e.english = readString(*sub, keyStr, "english", /*required=*/true);
        e.description = readString(*sub, keyStr, "description", /*required=*/false);

        if (auto arr = (*sub)["placeholders"].as_array()) {
            for (auto &&item : *arr) {
                auto s = item.value<std::string>();
                if (!s)
                    fail("key '" + keyStr + "': every entry in 'placeholders' must be a string");
                e.placeholders.push_back(*s);
            }
        }

        e.requiredMask = parsePlaceholders(keyStr, e.english, e.distinctCount);
        if (static_cast<int>(e.placeholders.size()) != e.distinctCount)
            fail("key '" + keyStr + "': 'placeholders' documents " + std::to_string(e.placeholders.size()) +
                 " item(s) but 'english' uses " + std::to_string(e.distinctCount) + " distinct placeholder(s)");

        entries.push_back(std::move(e));
    }

    // Deterministic, stable order (toml++ already iterates sorted by key, but be explicit).
    std::sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) { return a.key < b.key; });
    return entries;
}

// Validate every shipped translation file against the source catalog. A shipped language MUST be
// complete and consistent: each source key present with a non-empty translation whose {N} placeholder
// set matches the source, an `english` reference still equal to the source (no stale drift), and no
// stray/unknown keys. Reports every problem (not just the first) and returns non-zero so the build
// fails. This is the runtime acceptance rule (lenientMask) hoisted to build time — anything that would
// silently fall back to English, or ship a translation made against an English string that has since
// changed, is caught here instead.
int runValidate(const std::string &srcPath, const std::vector<std::string> &langPaths) {
    const std::vector<Entry> entries = loadSource(srcPath);
    std::map<std::string, std::uint64_t> sourceKeys;  // key -> required placeholder mask
    std::map<std::string, std::string> sourceEnglish; // key -> source English (drift sentinel)
    for (const auto &e : entries) {
        sourceKeys.emplace(e.key, e.requiredMask);
        sourceEnglish.emplace(e.key, e.english);
    }

    bool ok = true;
    auto err = [&](const std::string &file, const std::string &msg) {
        std::cerr << "localization_gen: " << file << ": " << msg << "\n";
        ok = false;
    };

    for (const auto &langPath : langPaths) {
        toml::table root;
        try {
            root = toml::parse_file(langPath);
        } catch (const toml::parse_error &e) {
            err(langPath, "parse error: " + std::string(e.description()));
            continue;
        }

        // Each shipped translation must declare its BCP 47 culture tag so plugins can follow ofs-ng's
        // language (the [_meta] table is reserved metadata, not a translatable string).
        if (const auto *meta = root["_meta"].as_table()) {
            auto tag = (*meta)["culture"].value<std::string>();
            if (!tag || tag->empty())
                err(langPath, "[_meta] is missing a non-empty 'culture' tag");
        } else {
            err(langPath, "missing required [_meta] table with a 'culture' tag");
        }

        std::set<std::string> present;
        for (auto &&[key, node] : root) {
            std::string k(key.str());
            if (k.starts_with('_'))
                continue; // reserved metadata table (e.g. [_meta]); validated above, not a string key
            const auto *sub = node.as_table();
            if (sub == nullptr) {
                err(langPath, "'" + k + "' is not a [table]");
                continue;
            }
            auto it = sourceKeys.find(k);
            if (it == sourceKeys.end()) {
                err(langPath, "unknown key '" + k + "' (not in strings.toml)");
                continue;
            }
            present.insert(k);

            // Anti-drift: the `english` field is a stored copy of the source string the translation was
            // made against. If it no longer matches, the source text changed and the translation is
            // stale — it would ship silently outdated. Require the reference to be present and current;
            // `tools/translations.py sync`/`apply` keep it in step.
            auto eng = (*sub)["english"].value<std::string>();
            if (!eng)
                err(langPath, "key '" + k + "': missing 'english' reference (run tools/translations.py sync)");
            else if (*eng != sourceEnglish[k])
                err(langPath, "key '" + k +
                                  "': stale — source English changed since this was translated; "
                                  "re-translate (tools/translations.py todo/apply) to refresh it");

            auto tr = (*sub)["translation"].value<std::string>();
            if (!tr) {
                err(langPath, "key '" + k + "': missing 'translation' field");
                continue;
            }
            if (std::all_of(tr->begin(), tr->end(),
                            [](char c) { return std::isspace(static_cast<unsigned char>(c)); })) {
                err(langPath, "key '" + k + "': empty translation (incomplete)");
                continue;
            }
            if (lenientMask(*tr) != it->second)
                err(langPath, "key '" + k + "': translation placeholders {N} do not match the source");
        }

        for (const auto &[k, mask] : sourceKeys)
            if (present.find(k) == present.end())
                err(langPath, "missing key '" + k + "'");
    }

    if (!ok) {
        std::cerr << "localization_gen: translation validation failed\n";
        return 1;
    }
    std::cout << "localization_gen: validated " << langPaths.size() << " translation file(s) against " << entries.size()
              << " source keys\n";
    return 0;
}

// Overwrite `path` only when `content` differs from what is already on disk. CMake's Ninja generator
// runs codegen custom commands with restat=1, so preserving an unchanged output's mtime prunes the
// downstream rebuild: editing only string *text* rewrites StringsGenerated.cpp but leaves the .h (just
// the Tr enum + Str:: constants) byte-identical, so the many TUs that include Translator.h are skipped.
void writeIfChanged(const std::string &path, const std::string &content) {
    {
        std::ifstream in(path, std::ios::binary);
        if (in) {
            const std::string existing((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (existing == content)
                return;
        }
    }
    std::ofstream out(path, std::ios::binary);
    if (!out)
        fail("cannot open output '" + path + "'");
    out << content;
}

void generate(const std::vector<Entry> &entries, const std::string &outHeader, const std::string &outSource) {
    const std::size_t count = entries.size() + 1; // + InvalidTr sentinel at index 0

    // ---- Header ----
    {
        std::ostringstream h;
        h << "#pragma once\n";
        h << "// AUTO-GENERATED by localization_gen. DO NOT EDIT. Source: localization/strings.toml\n";
        h << "// NOLINTBEGIN — generated code; clang-tidy is checked when this header is included by a linted TU.\n\n";
        h << "#include \"Localization/TrKey.h\"\n";
        h << "#include <cstdint>\n";
        h << "#include <string_view>\n\n";

        h << "// Index space. Reordering keys renumbers this harmlessly: code resolves through\n";
        h << "// Str:: constants and data through key names, never raw indices.\n";
        h << "enum class Tr : std::uint32_t {\n";
        h << "    InvalidTr = 0,\n";
        for (std::size_t i = 0; i < entries.size(); ++i)
            h << "    " << entries[i].key << " = " << (i + 1) << ",\n";
        h << "    MaxStringCount\n";
        h << "};\n\n";

        h << "// Call-site surface: ImGui::Button(Str::Ok), Str::Foo.id(\"x\"), Str::Bar.fmt(a, b).\n";
        h << "namespace Str {\n";
        for (std::size_t i = 0; i < entries.size(); ++i)
            h << "inline constexpr TrKey " << entries[i].key << "{" << (i + 1) << "};\n";
        h << "} // namespace Str\n\n";

        h << "namespace ofs::loc::gen {\n";
        h << "inline constexpr std::uint32_t Count = " << count << "; // includes InvalidTr at index 0\n\n";
        h << "extern const char *const Default[Count];      // English source text\n";
        h << "extern const char *const Description[Count];   // translator-facing description\n";
        h << "extern const char *const KeyName[Count];       // index -> key (TrToKey, for export)\n";
        h << "extern const std::uint32_t PlaceholderCount[Count];\n";
        h << "extern const char *const *const Placeholders[Count]; // per-key doc array or nullptr\n";
        h << "extern const std::uint64_t RequiredMask[Count];      // placeholder index bitmask\n\n";
        h << "std::uint32_t keyToTr(std::string_view key);   // key -> Tr index, 0 if unknown\n";
        h << "} // namespace ofs::loc::gen\n";
        h << "// NOLINTEND\n";
        writeIfChanged(outHeader, h.str());
    }

    // ---- Source ----
    {
        std::ostringstream c;
        c << "// AUTO-GENERATED by localization_gen. DO NOT EDIT. Source: localization/strings.toml\n\n";
        c << "#include \"Localization/StringsGenerated.h\"\n";
        c << "#include <algorithm>\n";
        c << "#include <string_view>\n\n";
        c << "namespace ofs::loc::gen {\n\n";

        auto emitStringArray = [&](const char *name, auto &&pick) {
            c << "const char *const " << name << "[Count] = {\n";
            c << "    \"\",\n"; // InvalidTr
            for (const auto &e : entries)
                c << "    \"" << cppEscape(pick(e)) << "\",\n";
            c << "};\n\n";
        };
        emitStringArray("Default", [](const Entry &e) { return e.english; });
        emitStringArray("Description", [](const Entry &e) { return e.description; });
        emitStringArray("KeyName", [](const Entry &e) { return e.key; });

        c << "const std::uint32_t PlaceholderCount[Count] = {\n    0,\n";
        for (const auto &e : entries)
            c << "    " << e.placeholders.size() << ",\n";
        c << "};\n\n";

        c << "const std::uint64_t RequiredMask[Count] = {\n    0,\n";
        for (const auto &e : entries)
            c << "    " << e.requiredMask << "ULL,\n";
        c << "};\n\n";

        // Per-key placeholder doc arrays.
        for (const auto &e : entries) {
            if (e.placeholders.empty())
                continue;
            c << "static const char *const ph_" << e.key << "[] = {";
            for (std::size_t i = 0; i < e.placeholders.size(); ++i) {
                if (i)
                    c << ", ";
                c << "\"" << cppEscape(e.placeholders[i]) << "\"";
            }
            c << "};\n";
        }
        c << "\nconst char *const *const Placeholders[Count] = {\n    nullptr,\n";
        for (const auto &e : entries) {
            if (e.placeholders.empty())
                c << "    nullptr,\n";
            else
                c << "    ph_" << e.key << ",\n";
        }
        c << "};\n\n";

        // key -> Tr, binary search over a sorted table (entries are already sorted by key).
        c << "namespace {\n";
        c << "struct KeyEntry {\n    std::string_view key;\n    std::uint32_t idx;\n};\n";
        c << "constexpr KeyEntry kSortedKeys[] = {\n";
        for (std::size_t i = 0; i < entries.size(); ++i)
            c << "    {\"" << cppEscape(entries[i].key) << "\", " << (i + 1) << "},\n";
        c << "};\n";
        c << "} // namespace\n\n";

        c << "std::uint32_t keyToTr(std::string_view key) {\n";
        c << "    const auto *begin = std::begin(kSortedKeys);\n";
        c << "    const auto *end = std::end(kSortedKeys);\n";
        c << "    const auto *it =\n";
        c << "        std::lower_bound(begin, end, key, [](const KeyEntry &e, std::string_view k) { return e.key < k; "
             "});\n";
        c << "    if (it != end && it->key == key)\n";
        c << "        return it->idx;\n";
        c << "    return 0;\n";
        c << "}\n\n";

        c << "} // namespace ofs::loc::gen\n";
        writeIfChanged(outSource, c.str());
    }

    std::cout << "localization_gen: wrote " << entries.size() << " keys to " << outHeader << " and " << outSource
              << "\n";
}

} // namespace

int main(int argc, char **argv) {
    // Validation mode: check shipped translations against the source. No codegen.
    //   localization_gen --validate <strings.toml> [lang.toml ...]
    if (argc >= 2 && std::string_view(argv[1]) == "--validate") {
        if (argc < 3) {
            std::cerr << "usage: localization_gen --validate <strings.toml> [lang.toml ...]\n";
            return 2;
        }
        const std::vector<std::string> langPaths(argv + 3, argv + argc);
        return runValidate(argv[2], langPaths);
    }

    // Codegen mode: emit StringsGenerated.{h,cpp} from the source catalog.
    //   localization_gen <strings.toml> <out.h> <out.cpp>
    if (argc != 4) {
        std::cerr << "usage: localization_gen <strings.toml> <out.h> <out.cpp>\n";
        return 2;
    }
    generate(loadSource(argv[1]), argv[2], argv[3]);
    return 0;
}
