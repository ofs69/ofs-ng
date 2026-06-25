#pragma once

#include <filesystem>

namespace ofs {

// Process-wide single-instance guard. Holds an exclusive OS lock on a file in the pref dir for the
// object's lifetime; a second process that constructs one on the same path cannot acquire it
// (acquired() == false), which is how the app refuses to start a second copy of itself.
//
// The lock is enforced by the kernel on an open handle/descriptor, so it is released automatically
// when the process exits — including a hard crash — and never leaves a stale lock that strands the
// next launch. The lock *file* may persist on disk; that is intentional and harmless (it is re-opened
// each launch). Acquisition fails closed only on a genuine conflict: any unrelated filesystem error
// (e.g. a permission problem creating the file) is treated as "no guard" and lets the app proceed,
// so a single-instance check can never become the reason the app won't launch at all.
class InstanceLock {
  public:
    explicit InstanceLock(const std::filesystem::path &lockFile);
    ~InstanceLock();

    InstanceLock(const InstanceLock &) = delete;
    InstanceLock &operator=(const InstanceLock &) = delete;
    InstanceLock(InstanceLock &&) = delete;
    InstanceLock &operator=(InstanceLock &&) = delete;

    // False only when another live instance already holds the lock. True both when we hold it and
    // when the lock could not be established at all (fail-open — see the class comment).
    bool acquired() const { return acquired_; }

  private:
    bool acquired_ = false;
#ifdef _WIN32
    void *handle_ = nullptr; // HANDLE; kept as void* so windows.h stays out of this header
#else
    int fd_ = -1;
#endif
};

} // namespace ofs
