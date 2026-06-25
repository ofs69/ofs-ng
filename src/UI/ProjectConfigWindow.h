#pragma once

#include "Core/FunscriptMetadata.h"

#include <string>
#include <unordered_map>
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
    void renderMetadataTab(const ScriptProject &project, EventQueue &eq);
    void renderCustomFields(FunscriptMetadata &meta, EventQueue &eq);
    void renderSettingsTab(const ScriptProject &project, EventQueue &eq, bool dummyPlayerActive);

    const AppSettings &appSettings;

    // Persistent editing mirror of project.metadata. ImGui InputText needs mutable buffers to bind to;
    // we re-sync from the document only when it actually changes (value compare, no allocation) instead
    // of deep-copying the whole struct every frame.
    FunscriptMetadata metaBuf;

    std::string newTagInput;
    std::string newPerformerInput;
    std::string newPresetName;
    int selectedPresetIdx = -1;

    // Custom-field editor state. newCustomFieldKey/Type back the "add field" row; jsonEdits holds the
    // in-progress raw-JSON text for array/object fields (keyed by field key) so edits survive frames
    // while the text is not yet valid JSON. `valid` caches the parse result so the per-frame render
    // never re-parses; `lastValue` detects external changes (preset load / reset) to reflow the text.
    struct JsonEditState {
        std::string text;
        nlohmann::json lastValue;
        bool valid = true;
    };
    std::string newCustomFieldKey;
    int newCustomFieldType = 0;
    std::unordered_map<std::string, JsonEditState> jsonEdits;

    std::string dummyDuration_ = "00:05:00";
    bool dummyDurationError = false;

    // Markers tab: persistent edit mirrors for the chapter/bookmark name inputs (ImGui InputText needs
    // stable buffers to bind to). Re-synced from the document only when an entry actually changes — the
    // same change-gated mirror pattern as metaBuf. Color needs no mirror: ColorEdit derives its value
    // from the document each frame.
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
