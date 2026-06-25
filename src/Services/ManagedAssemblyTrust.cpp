#include "Services/ManagedAssemblyTrust.h"
#include "Util/FileUtil.h"
#include "Util/Log.h"
#include "Util/PathUtil.h"
#include <OfsBakedHashes.h> // generated: baked SHA-256 of plugins + managed assemblies; changes on
                            // every plugin/managed-assembly byte change, so isolate it to this TU.
#include <algorithm>
#include <array>
#include <picosha2.h>
#include <ranges>

namespace ofs {

bool managedAssemblyTrusted(const std::filesystem::path &dll, std::string_view name) {
    std::string_view expected;
    for (const auto &m : generated::kManagedAssemblies)
        if (m.name == name) {
            expected = m.sha256;
            break;
        }
    if (expected.empty())
        return true;

    auto bytes = ofs::util::readFile(dll);
    if (!bytes) {
        OFS_CORE_ERROR("Cannot read managed assembly for integrity check: {}", ofs::util::toUtf8(dll));
        return false;
    }
    if (picosha2::hash256_hex_string(*bytes) != expected) {
        OFS_CORE_ERROR("Managed assembly {} failed integrity check (hash mismatch): {}", name, ofs::util::toUtf8(dll));
        return false;
    }
    return true;
}

bool firstPartyHashMatches(std::string_view name, std::string_view hash) {
    for (const auto &fp : generated::kFirstPartyPlugins)
        if (fp.name == name)
            return !fp.sha256.empty() && fp.sha256 == hash;
    return false;
}

bool isFirstPartyName(std::string_view name) {
    return std::ranges::any_of(generated::kFirstPartyPlugins,
                               [&](const generated::BakedHash &fp) { return fp.name == name; });
}

std::span<const std::string_view> firstPartyPluginNames() {
    static constexpr auto names = [] {
        std::array<std::string_view, generated::kFirstPartyPlugins.size()> a{};
        for (size_t i = 0; i < a.size(); ++i)
            a[i] = generated::kFirstPartyPlugins[i].name;
        return a;
    }();
    return names;
}

} // namespace ofs
