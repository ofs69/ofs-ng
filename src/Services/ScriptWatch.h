#pragma once

#include <filesystem>
#include <iterator>
#include <string>
#include <unordered_map>
#include <vector>

namespace ofs {

// Pure mtime-diff backing the script file-watcher (ScriptSystem::update). Given the files watched
// this tick mapped to their current on-disk mtimes, and the map of mtimes seen on the previous tick
// (mutated in place), this:
//   • prunes `lastSeen` entries for files no longer watched,
//   • records a file seen for the first time WITHOUT reporting it (baseline seed — the file was just
//     compiled on pick/create, so re-running it would be redundant), and
//   • reports (and updates the baseline for) every file whose mtime changed since last seen.
// Returns the changed files, i.e. the ones the caller should recompile. No filesystem access — the
// caller does the stat-ing, so this is deterministic and unit-testable with synthetic timestamps.
using WatchMtimeMap = std::unordered_map<std::string, std::filesystem::file_time_type>;

inline std::vector<std::string> diffWatchedMtimes(const WatchMtimeMap &current, WatchMtimeMap &lastSeen) {
    for (auto it = lastSeen.begin(); it != lastSeen.end();)
        it = current.contains(it->first) ? std::next(it) : lastSeen.erase(it);

    std::vector<std::string> changed;
    for (const auto &[file, mtime] : current) {
        auto [it, inserted] = lastSeen.try_emplace(file, mtime);
        if (inserted)
            continue; // first observation: seed the baseline, do not recompile
        if (it->second != mtime) {
            it->second = mtime;
            changed.push_back(file);
        }
    }
    return changed;
}

} // namespace ofs
