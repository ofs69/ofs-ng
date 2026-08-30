#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace ofs {

struct AppSettings;
class EventQueue;
struct ScriptProject;

// Why the endpoint could not start. A code, not a message: the window owns the wording so it goes
// through the localization catalog like every other user-visible string.
enum class WebSocketApiError : uint8_t {
    None,
    WinsockInit,
    CreateSocket,
    Listen,
};

// Passive main-thread snapshot for UI rendering. OfsApp reads it from WebSocketApi after update() and
// passes the value across the UI seam; windows never invoke the networking service directly.
struct WebSocketApiStatus {
    bool running = false;
    size_t clientCount = 0;
    WebSocketApiError error = WebSocketApiError::None;
    int errorPort = 0; // the port Listen failed on; meaningless for the other codes
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
