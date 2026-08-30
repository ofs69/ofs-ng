#pragma once

namespace ofs {

struct AppSettings;
class EventQueue;
struct WebSocketApiStatus;

class WebSocketApiWindow {
  public:
    void render(bool &open, const AppSettings &settings, const WebSocketApiStatus &status, EventQueue &eq);
};

} // namespace ofs
