#pragma once

#include <filesystem>
#include <string>

namespace ofs::test {

// Absolute path to a file under tests/fixtures/ (resolved from the OFS_TESTS_DIR compile define).
std::filesystem::path fixturePath(const std::string &name);

// Compare two .funscript files for action equality. The root "actions" array, funscript-1.1
// "axes" entries, and funscript-2.0 "channels" are all compared. Timestamps are normalized to
// whole milliseconds (3 decimal places of seconds) before comparison, so floating-point drift
// through a save/reload round-trip never produces a spurious mismatch.
//
// Returns true when equal. On mismatch returns false and, when `diff` is non-null, writes a
// human-readable description of the first difference for the test report.
bool compareFunscriptFiles(const std::filesystem::path &actual, const std::filesystem::path &expected,
                           std::string *diff = nullptr);

} // namespace ofs::test
