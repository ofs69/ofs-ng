#pragma once

#include "Util/PathUtil.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <spdlog/fmt/fmt.h>
#include <string>
#include <string_view>
#include <vector>

namespace ofs {

// Single source of truth for the media file types we hand to libmpv. Lowercase, leading dot.
// File-dialog filters and the "is this media?" checks both derive from these arrays, so the picker
// filter and the extension test can never drift apart. libmpv plays far more than this; the lists
// are the common containers worth surfacing in a file dialog.
inline constexpr std::array<std::string_view, 14> kVideoExtensions = {
    ".mp4", ".m4v", ".mkv", ".webm", ".mov", ".avi", ".wmv", ".flv", ".mpg", ".mpeg", ".ts", ".m2ts", ".ogv", ".3gp"};

inline constexpr std::array<std::string_view, 8> kAudioExtensions = {".mp3", ".m4a", ".aac",  ".flac",
                                                                     ".wav", ".ogg", ".opus", ".wma"};

// Lowercased UTF-8 extension of `path` including the leading dot ("" if none). ASCII-lowercasing
// suffices — every extension we match is ASCII.
inline std::string lowerExtension(const std::filesystem::path &path) {
    std::string ext = ofs::util::toUtf8(path.extension());
    std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

inline bool isVideoExtension(const std::filesystem::path &path) {
    const std::string ext = lowerExtension(path);
    return std::ranges::find(kVideoExtensions, ext) != kVideoExtensions.end();
}

inline bool isAudioExtension(const std::filesystem::path &path) {
    const std::string ext = lowerExtension(path);
    return std::ranges::find(kAudioExtensions, ext) != kAudioExtensions.end();
}

inline bool isMediaExtension(const std::filesystem::path &path) {
    return isVideoExtension(path) || isAudioExtension(path);
}

// Glob patterns ("*.mp4", …) for every media extension, as FileDialogSpec::filterPatterns expects.
// Rebuilt per call — dialog setup is never a hot path.
inline std::vector<std::string> mediaFilterPatterns() {
    std::vector<std::string> patterns;
    patterns.reserve(kVideoExtensions.size() + kAudioExtensions.size());
    for (const auto ext : kVideoExtensions)
        patterns.emplace_back(fmt::format("*{}", ext));
    for (const auto ext : kAudioExtensions)
        patterns.emplace_back(fmt::format("*{}", ext));
    return patterns;
}

} // namespace ofs
