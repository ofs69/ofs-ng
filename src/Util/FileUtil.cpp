#include "FileUtil.h"
#include "PathUtil.h"

#include <SDL3/SDL_iostream.h>

namespace ofs::util {

std::optional<std::string> readFile(const std::filesystem::path &path) {
    size_t size = 0;
    void *buf = SDL_LoadFile(toUtf8(path).c_str(), &size);
    if (!buf)
        return std::nullopt;
    std::string result(static_cast<const char *>(buf), size);
    SDL_free(buf);
    return result;
}

bool writeFile(const std::filesystem::path &path, std::string_view data) {
    return SDL_SaveFile(toUtf8(path).c_str(), data.data(), data.size());
}

bool writeFile(const std::filesystem::path &path, const void *data, size_t size) {
    return SDL_SaveFile(toUtf8(path).c_str(), data, size);
}

bool writeFileAtomic(const std::filesystem::path &path, const void *data, size_t size) {
    // Temp file sits next to the target so the rename stays on the same filesystem (where it is
    // atomic). The ".tmp" suffix is ASCII, so appending it never disturbs a non-ASCII path.
    std::filesystem::path tmp = path;
    tmp += ".tmp";

    if (!writeFile(tmp, data, size))
        return false;

    // std::filesystem::rename replaces an existing destination atomically (MoveFileEx with
    // MOVEFILE_REPLACE_EXISTING on Windows). On failure the original `path` is left intact.
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::error_code rmEc;
        std::filesystem::remove(tmp, rmEc);
        return false;
    }
    return true;
}

bool writeFileAtomic(const std::filesystem::path &path, std::string_view data) {
    return writeFileAtomic(path, data.data(), data.size());
}

} // namespace ofs::util
