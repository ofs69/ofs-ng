#pragma once

#include "Core/ProcessingRegion.h" // GraphNodeType (AddNodeRequest), ProcessingRegion (render helpers)
#include "Core/StandardAxis.h"
#include <cstdint>
#include <string>
#include <vector>

struct ImNodesEditorContext;
struct ImVec2;

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
struct ProcessingRegion;

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

  private:
    // One node-creation request produced by the add-node popup and consumed by the node-create branches
    // after EndNodeEditor (the two are separated by the imnodes editor scope, so the choice is carried
    // across rather than acted on in place).
    struct AddNodeRequest {
        float posX = 0.0f; // screen-space drop position (popup-open mouse pos)
        float posY = 0.0f;
        GraphNodeType type = GraphNodeType::Input; // native math/discretize/constant node to add
        bool addEffect = false;
        std::string effectType;
        bool addPlugin = false;
        std::string pluginNodeId;
        bool addScript = false; // a ready library/user script picked from the catalog
        int scriptIndex = -1;   // index into m_scriptCatalog
    };
    AddNodeRequest renderAddNodeMenu(const EffectRegistryState &effectReg);

    // Modal bodies raised from render(): each latches on a one-shot flag (New Script / Save embedded) or a
    // pending-graph-load condition (Trust / Remap), then hands a deferred [this]-capturing body to the
    // ModalManager. Split out of render() so the orchestrator stays readable; the bodies are unchanged.
    void renderHeader(const ScriptProject &project, EventQueue &eq, int selId, const ProcessingRegion &region);
    // Render one node's body (params / plugin UI) inside the imnodes editor. A member (not a free function)
    // so its per-frame buffers — the plugin-node TState scratch and the script-file combo cache — are reused
    // across frames rather than living in function statics.
    void renderNode(const ProcessingGraphNode &node, const EffectRegistryState &effectReg,
                    const ScriptRegistryState &scriptReg, EventQueue &eq, int regionId, const ProcessingRegion &region,
                    bool hot, bool pending, int &outSaveReqNodeId, int &outLoadReqNodeId);
    void renderFooter(const ScriptProject &project, EventQueue &eq, int selId, const ProcessingRegion &region,
                      bool anyPending);
    // Node/link right-click context menus (Duplicate / Disconnect / Delete). Begun inside the editor
    // scope alongside the add-node popup; acts on m_ctxNodeId / m_ctxLinkId captured when opened.
    void renderGraphContextMenus(const ProcessingRegion &region, EventQueue &eq, int selId, ImVec2 windowPad) const;
    void maybeShowNewScriptModal(EventQueue &eq);
    void maybeShowSaveScriptModal(EventQueue &eq);
    void maybeShowTrustModal(const ScriptProject &project, EventQueue &eq, int selId);
    void maybeShowRemapModal(const ScriptProject &project, EventQueue &eq, int selId, const ProcessingRegion &region);

    bool m_cursorInside = false; // cursor was over this panel (z-order aware) this frame; for click-away deselect
    bool m_graphFocused = false;
    ImNodesEditorContext *m_editorCtx = nullptr;
    int m_loadedRegionId = -1;
    std::string m_nameEdit;
    std::string m_nodeUiBuffer;                 // reused in/out buffer for a plugin node's TState JSON (renderNode)
    std::vector<std::string> m_scriptFileCache; // script-file combo entries; refreshed when the combo opens
    int m_scriptFileCacheNode = -1;             // node id m_scriptFileCache was built for
    int m_pendingLinkPin = -1;
    bool m_openAddNodeMenuNextFrame = false;
    std::string m_nodeFilter; // add-node menu fuzzy filter; std::string so localized input isn't byte-capped
    bool m_focusFilterNextFrame = false;

    // Node/link right-click context menu (transient): the graph element the open menu acts on, captured
    // when the menu is opened (the hovered node/link id, decoded to its owner record id).
    int m_ctxNodeId = -1;
    int m_ctxLinkId = -1;
    // Fit-to-view request, raised by the header button or the F shortcut and consumed after EndNodeEditor
    // (where node dimensions are known). Frames the current node selection, or the whole graph when none.
    bool m_fitRequested = false;

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
