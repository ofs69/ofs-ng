#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace ofs::util {

// True only in the test binaries, which are all compiled with OFS_TEST_PREF_SUBDIR (see
// tests/CMakeLists.txt). Like ofs::kHeadless it folds at compile time, so production keeps the real
// behavior with zero test-only state and no runtime setter. Used to suppress side effects that must
// never fire under test — e.g. launching an external editor from openInDefaultApp.
#ifdef OFS_TEST_PREF_SUBDIR
inline constexpr bool kIsTestBuild = true;
#else
inline constexpr bool kIsTestBuild = false;
#endif

// ── UTF-8 ⇄ path conversion ──────────────────────────────────────────────────
//
// Invariant for the whole codebase: every `std::string`/`const char*` that holds
// a path is UTF-8. `std::filesystem::path` is the lossless carrier (it stores
// wchar_t natively on Windows). These two functions are the ONLY sanctioned way
// to cross between the two representations.
//
// Do NOT use `path::string()` (it re-encodes to the Windows ANSI codepage and
// silently drops any character outside it) or the narrow `path(std::string)`
// constructor / `operator/` / `operator+=` with a narrow string (they interpret
// the bytes as ANSI, not UTF-8). Use these helpers instead. The rule is enforced
// by tools/check_path_encoding.py.

// path → UTF-8 std::string. Correct on every platform; preserves non-ASCII
// characters that path::string() would lose to the active codepage on Windows.
std::string toUtf8(const std::filesystem::path &p);

// UTF-8 std::string → path. Correct on every platform; interprets the bytes as
// UTF-8 rather than the Windows ANSI codepage.
std::filesystem::path fromUtf8(std::string_view utf8);

// Directory for app config/data. Test binaries are compiled with the
// OFS_TEST_PREF_SUBDIR define (see tests/CMakeLists.txt) so this resolves to a
// temp directory instead of the real app-data dir — no runtime override needed.
const std::filesystem::path &getPrefPath();

// Directory where crash dumps are written (a "crashes" subfolder of the pref path).
const std::filesystem::path &getCrashDir();

// Directory containing the running executable (where shipped assets like data/ and lang/ live).
const std::filesystem::path &getBasePath();

// Directory holding the shipped managed assemblies (getBasePath()/managed): the .NET host assemblies —
// Ofs.Api, Ofs.PluginHost, Ofs.ScriptHost, Ofs.HostServices — and the first-party plugins under
// managed/plugins. Anchored to the executable, never to the process working directory: the app is
// launched from an arbitrary cwd by a .desktop entry, an AppImage's AppRun, or a shell, and a relative
// "managed/…" resolves against that cwd, silently failing every assembly's trust check.
const std::filesystem::path &getManagedPath();

// Build a `file:///` URI from a path. Emits UTF-8 bytes verbatim (no percent-
// encoding — SDL_OpenURL / the OS shell accept raw UTF-8) and normalizes Windows
// '\\' separators to '/'. Exposed for testing; openInFileBrowser/openInDefaultApp
// hand the result to SDL_OpenURL.
std::string fileUri(const std::filesystem::path &p);

void openInFileBrowser(const std::filesystem::path &dir);

// Open an existing file in the OS default application (e.g. a .cs script in the default editor).
// Unlike openInFileBrowser it does NOT create the path — the file must already exist. A no-op in test
// binaries (see kIsTestBuild) so a flow that "opens" files never spawns an external program under test.
void openInDefaultApp(const std::filesystem::path &file);

// Reveal a file in the OS file browser. On Windows this launches Explorer with the file highlighted
// (selected); every other platform has no portable "select the file" verb, so it falls back to opening
// the file's containing directory. If the file no longer exists, opens its parent directory instead. A
// no-op in test binaries (see kIsTestBuild) so a UI flow never spawns Explorer/Finder under test.
void revealInFileBrowser(const std::filesystem::path &file);

} // namespace ofs::util
