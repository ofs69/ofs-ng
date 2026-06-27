#pragma once

#include "Services/UpdateChecker.h" // UpdateChecker::Status read-only for the Updates section

#include <string>

namespace ofs {

class EventQueue;

// About window: the app's build identity plus a credits list of every bundled third-party component,
// each with a click-to-view pane showing its full, verbatim license. Pure renderer — it owns only the
// transient selection and the cached license body. Build info is compile-time and the license texts load
// on demand from the asset archive (ofs::res); the only live data it reads is the update-check status,
// and the only event it pushes is a manual re-check (the Check Now button).
class AboutWindow {
  public:
    void render(bool &open, const UpdateChecker::Status &update, EventQueue &eq);

  private:
    void selectComponent(int index); // loads the component's license body into licenseText_

    int selected_ = -1;       // index into the attribution table; -1 = nothing selected yet
    std::string licenseText_; // cached body of the selected component's license (empty until selected)
};

} // namespace ofs
