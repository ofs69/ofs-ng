#pragma once

#include <string>

namespace ofs {

// About window: the app's build identity plus a credits list of every bundled third-party component,
// each with a click-to-view pane showing its full, verbatim license. Pure renderer — it owns only the
// transient selection and the cached license body. It needs no project or event-queue access: build
// info is compile-time, and the license texts are loaded on demand from the asset archive (ofs::res),
// the same files that ship inside data.pak.
class AboutWindow {
  public:
    void render(bool &open);

  private:
    void selectComponent(int index); // loads the component's license body into licenseText_

    int selected_ = -1;       // index into the attribution table; -1 = nothing selected yet
    std::string licenseText_; // cached body of the selected component's license (empty until selected)
};

} // namespace ofs
