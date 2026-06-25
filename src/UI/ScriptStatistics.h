#pragma once

namespace ofs {

struct ScriptProject;

class ScriptStatisticsWindow {
  public:
    ScriptStatisticsWindow() = default;
    void render(const ScriptProject &project, bool &open) const;
};

} // namespace ofs
