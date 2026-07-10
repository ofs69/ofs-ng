#pragma once

#include <string>
#include <vector>

namespace ofs {

struct AppSettings;
struct ScriptProject;
class EventQueue;

class ProjectConfigWindow {
  public:
    explicit ProjectConfigWindow(const AppSettings &appSettings);

    void render(const ScriptProject &project, EventQueue &eq, bool &open, double sessionTime, double mediaDuration,
                bool dummyPlayerActive);

  private:
    void renderInfoTab(const ScriptProject &project, double sessionTime, double mediaDuration);
    void renderMarkersTab(const ScriptProject &project, EventQueue &eq);
    void renderSettingsTab(const ScriptProject &project, EventQueue &eq, bool dummyPlayerActive);

    const AppSettings &appSettings;

    std::string dummyDuration_ = "00:05:00";
    bool dummyDurationError = false;

    // Markers tab: persistent edit mirrors for the chapter/bookmark name inputs (ImGui InputText needs
    // stable buffers to bind to). Re-synced from the document only when an entry actually changes (a
    // change-gated mirror, no per-frame copy). Color needs no mirror: ColorEdit derives its value from
    // the document each frame.
    std::vector<std::string> chNameBuf_;
    std::vector<std::string> bmNameBuf_;

    // Undo-gesture latch (mirrors BandBar/VideoPlayerControls): a name-typing or color-picker drag pushes
    // a live mutation every frame for real-state preview, but snapshots undo only on the gesture's first
    // change so the whole edit coalesces into one undo step. The (kind, index) pair scopes the gesture to
    // one field of one row, so moving to another row starts a fresh step.
    enum class MarkerEdit { None, ChapterName, ChapterColor, BookmarkName };
    MarkerEdit markerEdit_ = MarkerEdit::None;
    int markerEditIdx_ = -1;
};

} // namespace ofs
