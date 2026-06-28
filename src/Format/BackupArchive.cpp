#include "Format/BackupArchive.h"

#include "Util/PathUtil.h"

#include <algorithm>
#include <picosha2.h>
#include <spdlog/fmt/fmt.h>
#include <string_view>

namespace ofs::backup {

// Dated backups are named "backup-<sortable timestamp>.ofp" so a lexicographic sort is chronological —
// both pruning and the restore list rely on that ordering.
static constexpr std::string_view kPrefix = "backup-";

// A short, stable, filesystem-safe key for a project's full path. The readable stem prefix keeps the
// folder recognizable when browsing the backup directory by hand; the path hash disambiguates same-stem
// projects in different folders. weakly_canonical collapses "."/".." and relative segments so the same
// file reached two ways maps to one key; it falls back to the raw path if the file can't be resolved.
static std::string pathKey(const std::filesystem::path &projectFile) {
    std::error_code ec;
    std::filesystem::path canon = std::filesystem::weakly_canonical(projectFile, ec);
    const std::filesystem::path &p = ec ? projectFile : canon;
    std::string hash = picosha2::hash256_hex_string(ofs::util::toUtf8(p)).substr(0, 8);
    return fmt::format("{}-{}", ofs::util::toUtf8(p.stem()), hash);
}

// "backup-YYYY-MM-DD_HH-MM-SS.ofp" → "YYYY-MM-DD HH:MM:SS". Falls back to the raw core if the 19-char
// timestamp doesn't match (e.g. a hand-renamed file), so such files still list legibly.
static std::string displayName(const std::string &fileName) {
    std::string core = fileName.substr(kPrefix.size(), fileName.size() - kPrefix.size() - 4);
    if (core.size() == 19) {
        core[10] = ' ';
        core[13] = ':';
        core[16] = ':';
    }
    return core;
}

std::filesystem::path dirForProject(const std::filesystem::path &projectFile) {
    auto base = ofs::util::getPrefPath() / "backup";
    if (projectFile.empty())
        return base / "_unnamed_";
    return base / ofs::util::fromUtf8(pathKey(projectFile));
}

std::string fileName(std::time_t when) {
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &when);
#else
    localtime_r(&when, &tm);
#endif
    return fmt::format("{}{:04}-{:02}-{:02}_{:02}-{:02}-{:02}.ofp", kPrefix, tm.tm_year + 1900, tm.tm_mon + 1,
                       tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
}

std::vector<BackupFile> list(const std::filesystem::path &dir) {
    std::vector<BackupFile> out;
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file())
            continue;
        const auto &p = entry.path();
        std::string fn = ofs::util::toUtf8(p.filename());
        if (p.extension() != ".ofp" || !fn.starts_with(kPrefix))
            continue;
        out.push_back({.path = p, .display = displayName(fn), .bytes = entry.file_size(ec)});
    }
    // Sortable timestamp ⇒ lexicographic path order is chronological; descending puts newest first.
    std::ranges::sort(out, [](const BackupFile &a, const BackupFile &b) { return a.path > b.path; });
    return out;
}

void prune(const std::filesystem::path &dir, int keepCount) {
    keepCount = std::max(1, keepCount);
    std::error_code ec;
    std::vector<std::filesystem::path> files;
    for (const auto &entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file())
            continue;
        const auto &p = entry.path();
        if (p.extension() == ".ofp" && ofs::util::toUtf8(p.filename()).starts_with(kPrefix))
            files.push_back(p);
    }
    if (static_cast<int>(files.size()) <= keepCount)
        return;
    std::ranges::sort(files); // sortable timestamp ⇒ oldest first
    for (size_t i = 0; i + static_cast<size_t>(keepCount) < files.size(); ++i)
        std::filesystem::remove(files[i], ec);
}

} // namespace ofs::backup
