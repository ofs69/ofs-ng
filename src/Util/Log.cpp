#include "Log.h"
#include "PathUtil.h"

#include <atomic>
#include <deque>
#include <mutex>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/null_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <vector>

namespace ofs {
namespace {

// spdlog sink that keeps the most recent log lines in memory for the in-app Log window. A bounded
// ring (oldest dropped) so a long session can't grow without limit; a generation counter lets the
// window skip the snapshot copy on frames with no new output.
class MemoryLogSink : public spdlog::sinks::base_sink<std::mutex> {
  public:
    explicit MemoryLogSink(size_t cap) : capacity(cap) {}

    [[nodiscard]] uint64_t generation() const { return generationCounter.load(std::memory_order_relaxed); }

    void snapshot(std::vector<LogEntry> &out) {
        std::lock_guard<std::mutex> lock(mutex_);
        out.assign(entries.begin(), entries.end());
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        entries.clear();
        generationCounter.fetch_add(1, std::memory_order_relaxed);
    }

  protected:
    void sink_it_(const spdlog::details::log_msg &msg) override {
        spdlog::memory_buf_t formatted;
        formatter_->format(msg, formatted);
        std::string text(formatted.data(), formatted.size());
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
            text.pop_back();

        entries.push_back({.level = msg.level, .text = std::move(text)});
        if (entries.size() > capacity)
            entries.pop_front();
        generationCounter.fetch_add(1, std::memory_order_relaxed);
    }

    void flush_() override {}

  private:
    std::deque<LogEntry> entries;
    size_t capacity;
    std::atomic<uint64_t> generationCounter{0};
};

std::shared_ptr<MemoryLogSink> gMemorySink;

} // namespace

std::shared_ptr<spdlog::logger> Log::coreLogger =
    std::make_shared<spdlog::logger>("OFS", std::make_shared<spdlog::sinks::null_sink_mt>());

void Log::init() {
    // Co-locate the log with crash dumps so users can share both from one folder.
    // spdlog is built with SPDLOG_WCHAR_FILENAMES (see lib/CMakeLists.txt) so the file
    // sink takes the native path form, preserving non-ASCII characters in the pref dir.
    const std::filesystem::path logPath = ofs::util::getPrefPath() / "ofs.log";
#ifdef _WIN32
    const auto logFile = logPath.wstring();
#else
    const auto logFile = ofs::util::toUtf8(logPath); // native narrow encoding is UTF-8 off Windows
#endif

    // Rotating file sink with rotate-on-open: each launch renames ofs.log -> ofs.1.log -> ofs.2.log
    // (oldest discarded) before opening a fresh ofs.log, so the last three runs are retained. The
    // size cap is a safety valve against a single runaway session, not the primary rotation trigger.
    constexpr std::size_t kMaxLogFileSize = 50 * 1024 * 1024; // 50 MiB per run
    constexpr std::size_t kRotatedFiles = 2;                  // ofs.log + ofs.1.log + ofs.2.log = 3 files

    gMemorySink = std::make_shared<MemoryLogSink>(5000);

    std::vector<spdlog::sink_ptr> logSinks;
    logSinks.emplace_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
    logSinks.emplace_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
        logFile, kMaxLogFileSize, kRotatedFiles, /*rotate_on_open=*/true));
    logSinks.emplace_back(gMemorySink);

    logSinks[0]->set_pattern("%^[%T] %n: %v%$");
    logSinks[1]->set_pattern("[%T] [%l] %n: %v");
    logSinks[2]->set_pattern("[%T] [%l] %v");

    coreLogger = std::make_shared<spdlog::logger>("OFS", begin(logSinks), end(logSinks));
    spdlog::register_logger(coreLogger);
    coreLogger->set_level(spdlog::level::trace);
    coreLogger->flush_on(spdlog::level::trace);
}

uint64_t Log::logGeneration() {
    return gMemorySink ? gMemorySink->generation() : 0;
}

void Log::snapshotLog(std::vector<LogEntry> &out) {
    if (gMemorySink)
        gMemorySink->snapshot(out);
    else
        out.clear();
}

void Log::clearLog() {
    if (gMemorySink)
        gMemorySink->clear();
}
} // namespace ofs
