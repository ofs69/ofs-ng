#pragma once

#include "Core/StandardAxis.h"
#include <cstdint>
#include <string>
#include <vector>

struct ImNodesEditorContext;

namespace ofs {

// One ready-to-add script node in the add-node menu, built by scanning the user scripts folder and
// the shipped library and parsing each file's // !ofs: header. Rebuilt
// each time the add-node popup opens; never serialized.
struct ScriptCatalogEntry {
    std::string fileName;    // scriptFile reference (base name, e.g. "Sine.cs")
    std::string displayName; // !ofs:name, or the file stem
    std::string description; // !ofs:description (tooltip), may be empty
    uint8_t signal = 0;      // OfsSignalKind: 0 = functional, 1 = discrete
    int inputCount = 1;      // 0/1/2 → Generate / Modify / Combine (menu bucketing)
    int outputCount = 1;     // declared output pin count (seeds the node's scriptOutputCount cache)
    bool library = false;    // shipped read-only library vs. the user's own folder
};

struct ScriptProject;
class EventQueue;
struct EffectRegistryState;
struct ScriptRegistryState;

class ProcessingPanel {
  public:
    ProcessingPanel();
    ~ProcessingPanel();

    void render(const ScriptProject &project, EventQueue &eq, const EffectRegistryState &effectReg,
                const ScriptRegistryState &scriptReg);
    // Deletes the graph's selected nodes/links. Returns true if it consumed the delete (something was
    // selected), false if there was nothing to delete — letting the caller fall through to deleting the
    // region itself.
    bool deleteSelected(const ScriptProject &project, EventQueue &eq);

    [[nodiscard]] bool cursorInsideThisFrame() const { return m_cursorInside; }
    [[nodiscard]] bool isGraphFocused() const { return m_graphFocused; }
    [[nodiscard]] bool isLocked() const { return m_locked; }

  private:
    bool m_cursorInside = false; // cursor was over this panel (z-order aware) this frame; for click-away deselect
    bool m_graphFocused = false;
    bool m_locked = false; // when set, focus loss does not close the panel; transient, not persisted
    ImNodesEditorContext *m_editorCtx = nullptr;
    int m_loadedRegionId = -1;
    StandardAxis m_loadedAxisId = StandardAxis::Count;
    std::string m_nameEdit;
    int m_pendingLinkPin = -1;
    bool m_openAddNodeMenuNextFrame = false;
    std::string m_nodeFilter; // add-node menu fuzzy filter; std::string so localized input isn't byte-capped
    bool m_focusFilterNextFrame = false;

    // "New Script" creation dialog (transient). Picking Script from the add-node menu opens the
    // modal; on confirm the stub is written and a Script node is dropped at the captured position.
    bool m_openNewScriptModal = false; // request to OpenPopup on the next modal render
    bool m_focusScriptNameNextFrame = false;
    bool m_scriptCreateConfirmed = false; // consumed by the node-create block next frame
    float m_newScriptPosX = 0.0f;         // screen-space drop position captured at menu time
    float m_newScriptPosY = 0.0f;
    int m_newScriptLinkPin = -1; // link pin to auto-connect, or -1
    char m_newScriptName[96] = {};
    std::string m_newScriptDisplayName; // !ofs:name — add-menu display name (optional); std::string so localized text
                                        // isn't byte-capped
    std::string m_newScriptDescription; // !ofs:description — add-menu tooltip (optional)
    int m_newScriptSignal = 0;          // 0 = functional, 1 = discrete (OfsSignalKind)
    int m_newScriptInputs = 1;          // declared input pin count, 0..OFS_MAX_NODE_PINS
    int m_newScriptOutputs = 1;         // declared output pin count, 1..OFS_MAX_NODE_PINS
    bool m_newScriptComments = true;    // seed the new .cs with the verbose inline docs
    std::string m_newScriptFile;        // resolved file name once chosen (created stub or opened existing)
    std::vector<std::string> m_scriptFileList; // existing *.cs in the scripts folder; scanned on modal open

    // Add-menu script catalog (user folder + shipped library), rebuilt when the ##addnode popup opens.
    std::vector<ScriptCatalogEntry> m_scriptCatalog;

    // Graph-load remap dialog (transient): which pending load we've initialized for,
    // and the chosen target axis index per saved axis.
    int m_remapForRegion = -1;
    std::vector<int> m_remapTargets;

    // Graph-load trust dialog (transient): which pending load's embedded-scripts warning is open.
    int m_trustForRegion = -1;

    // "Save embedded script to scripts folder" dialog (transient): the node being promoted and the
    // name being typed. A collision with a *different* local file is rejected — the user must rename.
    bool m_openSaveScriptModal = false;
    int m_saveScriptRegionId = -1;
    int m_saveScriptNodeId = -1;
    std::string m_saveScriptName;
    std::string m_saveScriptSource; // captured at open, to detect a byte-identical existing file
};

} // namespace ofs
