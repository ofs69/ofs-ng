#include "InstanceLock.h"
#include "Log.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace ofs {

#ifdef _WIN32

InstanceLock::InstanceLock(const std::filesystem::path &lockFile) {
    // Share mode 0 (no sharing): the kernel grants this open to one process at a time. A second
    // process's CreateFileW on the same path fails with ERROR_SHARING_VIOLATION while we hold the
    // handle, and the handle is closed by the OS when this process dies — so the lock auto-clears on
    // a crash. The path's native form is wchar_t, so c_str() is the correct wide argument (no narrow
    // ANSI conversion). OPEN_ALWAYS creates the file if absent and opens an existing leftover.
    HANDLE h = CreateFileW(lockFile.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD err = GetLastError();
        if (err == ERROR_SHARING_VIOLATION) {
            acquired_ = false; // another live instance holds the lock
        } else {
            // Fail open: an unrelated FS error must never be the reason the app won't start.
            OFS_CORE_WARN("InstanceLock: could not open lock file (error {}); single-instance guard disabled", err);
            acquired_ = true;
        }
        return;
    }
    handle_ = h;
    acquired_ = true;
}

InstanceLock::~InstanceLock() {
    if (handle_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(handle_));
    }
}

#else

InstanceLock::InstanceLock(const std::filesystem::path &lockFile) {
    // On POSIX the path's native encoding is the byte string itself (UTF-8 on Linux/macOS), so
    // c_str() is the correct argument here with no conversion.
    const int fd = ::open(lockFile.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    if (fd < 0) {
        OFS_CORE_WARN("InstanceLock: could not open lock file (errno {}); single-instance guard disabled", errno);
        acquired_ = true; // fail open — see the Windows branch / class comment
        return;
    }

    // Advisory exclusive lock, non-blocking. flock is tied to the open file description and released
    // by the kernel when the process exits, so it self-clears on a crash. EWOULDBLOCK is the one and
    // only "another instance holds it" signal; any other error falls through to fail-open.
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        if (errno == EWOULDBLOCK) {
            ::close(fd);
            acquired_ = false;
            return;
        }
        OFS_CORE_WARN("InstanceLock: could not lock file (errno {}); single-instance guard disabled", errno);
        ::close(fd);
        acquired_ = true;
        return;
    }
    fd_ = fd;
    acquired_ = true;
}

InstanceLock::~InstanceLock() {
    if (fd_ >= 0) {
        ::close(fd_); // closing the descriptor releases the flock
    }
}

#endif

} // namespace ofs
