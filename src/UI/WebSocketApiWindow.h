#pragma once

namespace ofs {

struct AppSettings;
class EventQueue;
class WebSocketApi;

class WebSocketApiWindow {
  public:
    void render(bool &open, const AppSettings &settings, const WebSocketApi &api, EventQueue &eq);
};

} // namespace ofs
