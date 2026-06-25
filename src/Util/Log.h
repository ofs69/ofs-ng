#pragma once

#include <cstdint>
#include <memory>
#include <spdlog/fmt/ostr.h>
#include <spdlog/spdlog.h>
#include <string>
#include <vector>

namespace ofs {

// One captured log line, surfaced to the in-app Log window. `level` is the spdlog level so the
// window can colour by severity; `text` is the line already run through the sink's pattern formatter
// (timestamp + level + message), trailing newline stripped.
struct LogEntry {
    spdlog::level::level_enum level = spdlog::level::info;
    std::string text;
};

class Log {
  public:
    static void init();

    inline static std::shared_ptr<spdlog::logger> &getCoreLogger() { return coreLogger; }

    // In-app Log window access to the in-memory ring buffer. `logGeneration()` bumps on every appended
    // line and on clear, so the window only re-copies the buffer on frames where it actually changed
    // (keeping the per-frame render path allocation-free). All three are thread-safe — log lines arrive
    // from worker threads, the window reads on the main thread.
    static uint64_t logGeneration();
    static void snapshotLog(std::vector<LogEntry> &out);
    static void clearLog();

  private:
    static std::shared_ptr<spdlog::logger> coreLogger;
};
} // namespace ofs

#define OFS_CORE_TRACE(...) ::ofs::Log::getCoreLogger()->trace(__VA_ARGS__)
#define OFS_CORE_INFO(...) ::ofs::Log::getCoreLogger()->info(__VA_ARGS__)
#define OFS_CORE_WARN(...) ::ofs::Log::getCoreLogger()->warn(__VA_ARGS__)
#define OFS_CORE_ERROR(...) ::ofs::Log::getCoreLogger()->error(__VA_ARGS__)
#define OFS_CORE_CRITICAL(...) ::ofs::Log::getCoreLogger()->critical(__VA_ARGS__)
