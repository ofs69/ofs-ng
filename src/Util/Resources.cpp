#include "Resources.h"
#include "Log.h"
#include "PathUtil.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <miniz.h>
#include <mutex>

namespace ofs::res {
namespace {

std::optional<std::string> readWholeFile(const std::filesystem::path &path) {
    std::ifstream f(path, std::ios::binary); // utf8-ok: path carrier is wide on Windows
    if (!f)
        return std::nullopt;
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// The staged data.pak, loaded into memory once. miniz can be built without stdio (MINIZ_NO_STDIO),
// so we read the file ourselves and use the in-memory reader rather than mz_zip_reader_init_file. An
// empty buffer means "no archive next to the executable" — callers then use the source-tree fallback.
const std::string &archiveBytes() {
    static const std::string bytes = []() -> std::string {
        const std::filesystem::path zip = ofs::util::getBasePath() / ofs::util::fromUtf8("data.pak");
        return readWholeFile(zip).value_or(std::string{});
    }();
    return bytes;
}

std::optional<std::vector<std::byte>> readFromArchive(std::string_view name) {
    const std::string &bytes = archiveBytes();
    if (bytes.empty())
        return std::nullopt;

    mz_zip_archive zip{};
    if (mz_zip_reader_init_mem(&zip, bytes.data(), bytes.size(), 0) == MZ_FALSE)
        return std::nullopt;

    const std::string key(name); // mz_zip_reader_locate_file needs a NUL-terminated name
    const int idx = mz_zip_reader_locate_file(&zip, key.c_str(), nullptr, 0);
    if (idx < 0) {
        mz_zip_reader_end(&zip);
        return std::nullopt;
    }

    size_t outSize = 0;
    void *p = mz_zip_reader_extract_to_heap(&zip, static_cast<mz_uint>(idx), &outSize, 0);
    mz_zip_reader_end(&zip);
    if (p == nullptr)
        return std::nullopt;

    const auto *first = static_cast<const std::byte *>(p);
    std::vector<std::byte> out(first, first + outSize);
    mz_free(p);
    return out;
}

std::optional<std::vector<std::byte>> readFromFallback(std::string_view name) {
#ifdef OFS_ASSETS_FALLBACK_DIR
    const std::filesystem::path path =
        ofs::util::fromUtf8(OFS_ASSETS_FALLBACK_DIR) / ofs::util::fromUtf8(std::string(name));
    auto text = readWholeFile(path);
    if (!text)
        return std::nullopt;
    const auto *first = reinterpret_cast<const std::byte *>(text->data());
    return std::vector<std::byte>(first, first + text->size());
#else
    (void)name;
    return std::nullopt;
#endif
}

// One lock guards both the lazy archive load and every read/list, keeping the reader usable from any
// thread even though assets are normally fetched on the main thread at load time.
std::mutex &mutex() {
    static std::mutex m;
    return m;
}

} // namespace

std::optional<std::vector<std::byte>> read(std::string_view name) {
    std::lock_guard lock(mutex());
    if (auto v = readFromArchive(name))
        return v;
    if (auto v = readFromFallback(name))
        return v;
    OFS_CORE_ERROR("Resource not found: {}", name);
    return std::nullopt;
}

std::optional<std::string> readText(std::string_view name) {
    auto bytes = read(name);
    if (!bytes)
        return std::nullopt;
    return std::string(reinterpret_cast<const char *>(bytes->data()), bytes->size());
}

std::vector<std::string> list(std::string_view prefix) {
    std::lock_guard lock(mutex());
    std::vector<std::string> out;

    const std::string &bytes = archiveBytes();
    if (!bytes.empty()) {
        mz_zip_archive zip{};
        if (mz_zip_reader_init_mem(&zip, bytes.data(), bytes.size(), 0) != MZ_FALSE) {
            const mz_uint count = mz_zip_reader_get_num_files(&zip);
            for (mz_uint i = 0; i < count; ++i) {
                mz_zip_archive_file_stat stat;
                if (mz_zip_reader_file_stat(&zip, i, &stat) != MZ_FALSE &&
                    std::string_view(stat.m_filename).starts_with(prefix))
                    out.emplace_back(stat.m_filename);
            }
            mz_zip_reader_end(&zip);
        }
        if (!out.empty())
            return out;
    }

#ifdef OFS_ASSETS_FALLBACK_DIR
    const std::filesystem::path root = ofs::util::fromUtf8(OFS_ASSETS_FALLBACK_DIR);
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(root / ofs::util::fromUtf8(std::string(prefix)), ec), end;
         it != end; it.increment(ec)) {
        if (ec)
            break;
        if (!it->is_regular_file(ec))
            continue;
        std::string rel = ofs::util::toUtf8(std::filesystem::relative(it->path(), root, ec));
        std::ranges::replace(rel, '\\', '/'); // archive names use forward slashes
        out.push_back(std::move(rel));
    }
#endif
    return out;
}

} // namespace ofs::res
