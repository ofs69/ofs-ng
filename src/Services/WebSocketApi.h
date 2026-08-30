#pragma once

#include <cstddef>
#include <memory>
#include <string>

namespace ofs {

struct AppSettings;
class EventQueue;
struct ScriptProject;

// Passive main-thread snapshot for UI rendering. OfsApp reads it from WebSocketApi after update() and
// passes the value across the UI seam; windows never invoke the networking service directly.
struct WebSocketApiStatus {
    bool running = false;
    size_t clientCount = 0;
    std::string error;
};

// Classic OFS-compatible WebSocket endpoint. Networking is polled non-blocking from update(), so all
// project reads and command dispatch remain on the main thread and no untracked worker is required.
class WebSocketApi {
  public:
    WebSocketApi(ScriptProject &project, EventQueue &eq, const AppSettings &settings);
    ~WebSocketApi();

    WebSocketApi(const WebSocketApi &) = delete;
    WebSocketApi &operator=(const WebSocketApi &) = delete;

    void update(float dt);

    [[nodiscard]] const WebSocketApiStatus &status() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace ofs
