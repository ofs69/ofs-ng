#pragma once

#include "Util/Log.h"
#include "imgui.h"
#include <cstdint>
#include <vector>

namespace ofs {

// Live view of the in-memory log ring (see ofs::Log). Pure renderer: it owns only its own display
// cache and filter widgets — no project state. The cache is refreshed from ofs::Log only when the
// log's generation counter advances, so an open-but-idle window costs nothing per frame.
class LogWindow {
  public:
    void render(bool &open);

  private:
    void rebuildFiltered();

    std::vector<LogEntry> entries_;  // snapshot of the ring, refreshed on generation change
    std::vector<int> filtered_;      // indices into entries_ passing the level + text filter
    uint64_t cachedGeneration_ = ~0; // forces a snapshot on first render
    bool filterDirty_ = true;

    ImGuiTextFilter textFilter_;
    int minLevel_ = 0; // index into the level-filter combo (0 = all)
    bool autoScroll_ = true;
};

} // namespace ofs
