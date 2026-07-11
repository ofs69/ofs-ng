#pragma once

#include "Core/FunscriptMetadata.h"

#include <string>
#include <unordered_map>

namespace ofs {

struct AppSettings;
struct ScriptProject;
class EventQueue;

// Standalone editor for the project's funscript metadata (title/creator/URLs/license, description, notes,
// tags, performers, presets, custom fields). Formerly the Project window's "Metadata" tab; split into its
// own window so it can be popped directly (Edit ▸ Metadata, the open-metadata command, or auto-on-new-project).
class MetadataWindow {
  public:
    explicit MetadataWindow(const AppSettings &appSettings);

    void render(const ScriptProject &project, EventQueue &eq, bool &open);

  private:
    void renderCustomFields(FunscriptMetadata &meta, EventQueue &eq);

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
};

} // namespace ofs
