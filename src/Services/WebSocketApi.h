#pragma once

#include <cstddef>
#include <memory>
#include <string>

namespace ofs {

struct AppSettings;
class EventQueue;
struct ScriptProject;

// Classic OFS-compatible WebSocket endpoint. Networking is polled non-blocking from update(), so all
// project reads and command dispatch remain on the main thread and no untracked worker is required.
class WebSocketApi {
  public:
    WebSocketApi(ScriptProject &project, EventQueue &eq, const AppSettings &settings);
    ~WebSocketApi();

    WebSocketApi(const WebSocketApi &) = delete;
    WebSocketApi &operator=(const WebSocketApi &) = delete;

    void update(float dt);

    [[nodiscard]] bool isRunning() const;
    [[nodiscard]] size_t clientCount() const;
    [[nodiscard]] const std::string &lastError() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace ofs
