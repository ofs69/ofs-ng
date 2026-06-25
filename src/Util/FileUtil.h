#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace ofs::util {

std::optional<std::string> readFile(const std::filesystem::path &path);

// Write data to a file, replacing any existing content. Returns false on failure.
bool writeFile(const std::filesystem::path &path, std::string_view data);
bool writeFile(const std::filesystem::path &path, const void *data, size_t size);

// Write data atomically: write to a sibling temp file, then rename it over `path`. A crash,
// disk-full, or partial write leaves the original file untouched instead of a truncated/corrupt
// result. Returns false on failure (and removes the temp file). Use for documents that must never
// be left half-written, e.g. project (.ofp) saves.
bool writeFileAtomic(const std::filesystem::path &path, std::string_view data);
bool writeFileAtomic(const std::filesystem::path &path, const void *data, size_t size);

} // namespace ofs::util
