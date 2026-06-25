#include "PathUtil.h"
#include <SDL3/SDL.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

// Separate include block (blank line above): shellapi.h depends on windows.h's types, so it must come
// after it. clang-format sorts within a block and would otherwise alphabetize shellapi.h ahead.
#include <shellapi.h> // ShellExecuteW — reveal a file in Explorer via the /select switch
#endif

namespace ofs::util {

std::string toUtf8(const std::filesystem::path &p) {
    const std::u8string s = p.u8string();
    return {reinterpret_cast<const char *>(s.data()), s.size()};
}

std::filesystem::path fromUtf8(std::string_view utf8) {
    // utf8-ok: constructing from a u8string is the lossless UTF-8 path constructor.
    return {std::u8string(reinterpret_cast<const char8_t *>(utf8.data()), utf8.size())};
}

const std::filesystem::path &getPrefPath() {
    static const std::filesystem::path prefPath = []() -> std::filesystem::path {
#ifdef OFS_TEST_PREF_SUBDIR
        // Test binaries are compiled with OFS_TEST_PREF_SUBDIR so they resolve to a
        // temp directory at compile time. This keeps test config (settings.json,
        // imgui.ini, lastProjectPaths, …) completely separate from the real app data
        // dir with no runtime ordering to get wrong.
        return std::filesystem::temp_directory_path() / OFS_TEST_PREF_SUBDIR;
#else
        char *prefPathRaw = SDL_GetPrefPath("ofs", "ofs-ng");
        // SDL returns UTF-8 on every platform — decode it as UTF-8, not the ANSI codepage.
        std::filesystem::path dir = fromUtf8(prefPathRaw ? prefPathRaw : ".");
        SDL_free(prefPathRaw);
        return dir;
#endif
    }();
    return prefPath;
}

const std::filesystem::path &getCrashDir() {
    static const std::filesystem::path crashDir = getPrefPath() / "crashes";
    return crashDir;
}

const std::filesystem::path &getBasePath() {
    static const std::filesystem::path basePath = []() {
        const char *baseRaw = SDL_GetBasePath();
        // SDL returns UTF-8 — decode it as UTF-8, not the ANSI codepage.
        return fromUtf8(baseRaw != nullptr ? baseRaw : ".");
    }();
    return basePath;
}

std::string fileUri(const std::filesystem::path &p) {
    std::string uri = "file:///";
    for (char c : toUtf8(p))
        uri += (c == '\\') ? '/' : c;
    return uri;
}

void openInFileBrowser(const std::filesystem::path &dir) {
    std::filesystem::create_directories(dir);
    SDL_OpenURL(fileUri(dir).c_str());
}

void openInDefaultApp(const std::filesystem::path &file) {
    if constexpr (kIsTestBuild) {
        // A test binary must never spawn an external program; the file-writing that precedes the open
        // is what's under test, so the open itself folds away to nothing here.
        (void)file;
    } else {
        SDL_OpenURL(fileUri(file).c_str());
    }
}

void revealInFileBrowser(const std::filesystem::path &file) {
    if constexpr (kIsTestBuild) {
        // Like openInDefaultApp: a test binary must never spawn the OS file browser.
        (void)file;
    } else {
#ifdef _WIN32
        if (std::filesystem::exists(file)) {
            // explorer's /select wants '\' separators and the path as a single quoted token. Pass the
            // wide path straight to ShellExecuteW so a non-ASCII path survives (no lossy ANSI round-trip).
            std::filesystem::path selected = file;
            selected.make_preferred();
            const std::wstring params = L"/select,\"" + selected.native() + L"\"";
            ShellExecuteW(nullptr, L"open", L"explorer.exe", params.c_str(), nullptr, SW_SHOWNORMAL);
        } else {
            // The file is gone — open the nearest existing ancestor so the action still lands somewhere.
            openInFileBrowser(file.parent_path());
        }
#else
        // No portable shell verb selects a file, so open its containing directory instead.
        const std::filesystem::path dir = file.parent_path();
        SDL_OpenURL(fileUri(dir.empty() ? file : dir).c_str());
#endif
    }
}

} // namespace ofs::util
