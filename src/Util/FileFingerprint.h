#pragma once

#include <filesystem>
#include <string>

namespace ofs::util {

// Content fingerprint of a (possibly multi-GB) media file via a fast partial read: SHA-256 over the
// first, middle, and last 4 KiB plus the file size folded in. Files smaller than 12 KiB are hashed
// whole (size still folds in). Reads a fixed ~12 KiB regardless of length, so it is effectively
// instant on large videos. Returns the lowercase hex digest, or "" if the file cannot be read.
std::string fastFileFingerprint(const std::filesystem::path &path);

// Deterministic output path for the intra-optimized copy of `source`, placed in the shared `outDir`:
//   <fingerprint>.mp4   (just the content hash + the always-mp4 container extension)
// Content-based, so a moved/renamed source maps to the same output (reuse survives it). Returns an
// empty path if `source` cannot be fingerprinted.
std::filesystem::path intraOutputPath(const std::filesystem::path &outDir, const std::filesystem::path &source);

} // namespace ofs::util
