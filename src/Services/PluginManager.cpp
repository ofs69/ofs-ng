#include "PluginManager.h"
#include "Core/EventQueue.h"
#include "Core/ScriptProject.h"
#include "Format/Funscript.h"
#include "Localization/Translator.h"
#include "Services/ManagedAssemblyTrust.h" // verify managed host DLLs against baked-in hashes
#include "Services/PluginNodeIO.h"
#include "UI/ImGuiHelpers.h"
#include "UI/Modals.h"
#include "Util/Coro.h"
#include "Util/FileUtil.h"
#include "Util/FrameAllocator.h"
#include "Util/Log.h"
#include "Util/PathUtil.h"
#include "Video/VideoPlayer.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <format>
#include <imgui.h>
#include <imgui_internal.h>
#include <miniz.h>
#include <nlohmann/json.hpp>
#include <optional>
#include <picosha2.h>
#include <ranges>
#include <set>
#include <span>
#include <system_error>
#include <thread>
#include <vector>

namespace ofs {

namespace {

// Bytes of `s` that fit in `maxBytes` without splitting a UTF-8 codepoint. UTF-8 continuation bytes are
// 10xxxxxx (0x80–0xBF); if the byte limit lands mid-sequence we retreat past the dangling continuation
// bytes so the copied prefix is always whole — honouring the "never splits a codepoint" promise the
// read-back getters make in PluginApi.h. Result is in [0, min(s.size(), maxBytes)].
int utf8ClampLen(std::string_view s, int maxBytes) {
    int n = std::min(static_cast<int>(s.size()), std::max(maxBytes, 0));
    if (n < static_cast<int>(s.size()))
        while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80)
            --n;
    return n;
}

// Copies a UTF-8 string into a plugin buffer and returns the full required byte length (excl NUL); see
// the definition below. Forward-declared because the player/data-dir getters above the definition use it.
int fillBuf(char *buf, int bufSize, std::string_view s);

// Clear a string getter's out-buffer and report zero required length. Used on every early-return path so
// the managed GrowAndRead never reads stale ArrayPool bytes after a failed read. Forward-declared like
// fillBuf for the getters defined above it.
int emptyBuf(char *buf, int bufSize);

// Extract a .zip into `destDir` using miniz. Reads the archive into memory and writes each entry
// through ofs::util::writeFile so non-ASCII output paths survive on Windows (miniz's extract-to-file
// helper opens via the narrow/ANSI CRT). Rejects absolute or "../"-escaping entries (zip-slip).
// Returns false on any read/extract error.
bool extractZip(const std::filesystem::path &zipPath, const std::filesystem::path &destDir) {
    const std::string zipUtf8 = ofs::util::toUtf8(zipPath);
    auto bytes = ofs::util::readFile(zipPath);
    if (!bytes) {
        OFS_CORE_ERROR("Cannot read plugin zip: {}", zipUtf8);
        return false;
    }

    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_mem(&zip, bytes->data(), bytes->size(), 0)) {
        OFS_CORE_ERROR("Not a valid zip archive: {}", zipUtf8);
        return false;
    }

    bool ok = true;
    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < count && ok; ++i) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st)) {
            ok = false;
            break;
        }
        const std::filesystem::path rel = ofs::util::fromUtf8(st.m_filename); // miniz returns UTF-8 names
        bool unsafe = rel.is_absolute();
        for (const auto &part : rel)
            if (part == "..")
                unsafe = true;
        if (unsafe) {
            OFS_CORE_ERROR("Rejecting unsafe zip entry: {}", std::string(st.m_filename));
            ok = false;
            break;
        }

        const std::filesystem::path out = destDir / rel;
        std::error_code ec;
        if (mz_zip_reader_is_file_a_directory(&zip, i)) {
            std::filesystem::create_directories(out, ec);
            continue;
        }
        std::filesystem::create_directories(out.parent_path(), ec);
        size_t sz = 0;
        void *p = mz_zip_reader_extract_to_heap(&zip, i, &sz, 0);
        if (!p || !ofs::util::writeFile(out, p, sz))
            ok = false;
        if (p)
            mz_free(p);
    }

    mz_zip_reader_end(&zip);
    return ok;
}

// Move a directory tree to `dest`. Tries rename first (fast, same volume); on a cross-volume failure
// (temp and %APPDATA% are typically different volumes) falls back to a recursive copy + remove.
bool moveDir(const std::filesystem::path &src, const std::filesystem::path &dest) {
    std::error_code ec;
    std::filesystem::rename(src, dest, ec);
    if (!ec)
        return true;
    ec.clear();
    std::filesystem::copy(src, dest, std::filesystem::copy_options::recursive, ec);
    if (ec) {
        OFS_CORE_ERROR("Failed to move plugin into place: {}", ec.message());
        return false;
    }
    std::filesystem::remove_all(src, ec);
    return true;
}

// Delete a directory, retrying briefly. A just-unloaded plugin's DLL is normally released
// synchronously (UnloadPluginNative forces the collectible load context to be collected), but the OS
// may finish closing the image handle a beat later; a few short retries cover that lag without a
// perceptible stall. Returns true once the directory is gone.
bool removeDirRetry(const std::filesystem::path &dir) {
    std::error_code ec;
    for (int attempt = 0; attempt < 10; ++attempt) {
        std::filesystem::remove_all(dir, ec);
        if (!std::filesystem::exists(dir))
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return !std::filesystem::exists(dir);
}

// Within an extracted zip tree, find the plugin folder: a directory D that directly contains
// "D-name.dll" (the loader's "<name>/<name>.dll" shape). Checks the root, then its immediate
// children. Returns empty if none qualifies.
std::filesystem::path locatePluginDir(const std::filesystem::path &root) {
    auto holdsEntryDll = [](const std::filesystem::path &dir) {
        std::filesystem::path dll = dir / dir.filename();
        dll += ofs::util::fromUtf8(".dll");
        return std::filesystem::exists(dll);
    };
    if (holdsEntryDll(root))
        return root;
    std::error_code ec;
    for (const auto &e : std::filesystem::directory_iterator(root, ec))
        if (e.is_directory() && holdsEntryDll(e.path()))
            return e.path();
    return {};
}

PluginCtx *pc(void *ctx) {
    return static_cast<PluginCtx *>(ctx);
}

// Sets PluginCtx::currentPluginName for the duration of a single plugin callback, then clears it, so any
// host call the callback makes that is namespaced by the calling plugin (per-project data, and anything
// else keyed off currentPluginName) attributes to the right plugin. onLoad and onBuildUI set the name
// themselves (they also set per-pass UI state); every other callback dispatch wraps itself in one of these
// — without it currentPluginName is null outside onLoad/onBuildUI, so e.g. reading project data from
// onProjectChange would see no plugin.
struct CurrentPluginScope {
    PluginCtx &ctx;
    CurrentPluginScope(PluginCtx &c, const char *name) : ctx(c) { ctx.currentPluginName = name; }
    ~CurrentPluginScope() { ctx.currentPluginName = nullptr; }
    CurrentPluginScope(const CurrentPluginScope &) = delete;
    CurrentPluginScope &operator=(const CurrentPluginScope &) = delete;
};

// An axis is exposed to plugins when it is part of the project — shown in the strip, holding data, or
// locked. This is the document-existence predicate (AxisState::exists), the same one the save filter
// uses, so a plugin sees exactly the axes that persist. A standard axis hidden with data still counts:
// the data is real, so plugins can enumerate and write it even while it draws no strip row.
bool axisExists(const ScriptProject &project, size_t i) {
    return project.axes[i].exists();
}

// Copy up to bufSize actions from a sorted set into the plugin's flat buffer, returning the full count
// (so the plugin can re-query with a larger buffer). A null buf is a count-only probe.
int copyActionsToBuffer(const VectorSet<ScriptAxisAction> &src, PluginAction *buf, int bufSize) {
    const int count = static_cast<int>(src.size());
    if (buf) {
        const int toCopy = std::min(count, bufSize);
        auto it = src.begin();
        for (int i = 0; i < toCopy; ++i, ++it) {
            buf[i].at = it->at;
            buf[i].pos = it->pos;
        }
    }
    return count;
}

// Resolve a plugin-supplied axis role to its AxisState, or null if the role is out of range or the axis
// is not part of the project. Centralizes the bounds + existence gate every axis HostApi callback shares;
// write callbacks combine it with their own !eventQueue check.
const AxisState *validAxis(const PluginCtx *p, int role) {
    if (!p->project || role < 0 || role >= static_cast<int>(kStandardAxisCount))
        return nullptr;
    const auto i = static_cast<size_t>(role);
    return axisExists(*p->project, i) ? &p->project->axes[i] : nullptr;
}

// Bounds-checked element access for the indexed collection getters: the element at `index`, or null when
// out of range.
template <typename Coll> const typename Coll::value_type *itemAt(const Coll &coll, int index) {
    if (index < 0 || index >= static_cast<int>(coll.size()))
        return nullptr;
    return &coll[static_cast<size_t>(index)];
}

bool checkOnLoad(void *ctx, const char *fnName) {
    if (pc(ctx)->inOnLoad)
        return true;
    OFS_CORE_ERROR("[Plugin] '{}' must be called from onLoad, not at runtime.", fnName);
    return false;
}

bool checkMainThread(void *ctx, const char *fnName) {
    if (std::this_thread::get_id() == pc(ctx)->mainThreadId)
        return true;
    OFS_CORE_ERROR("[Plugin] '{}' called from a non-main thread. "
                   "All HostApi calls must be made on the main thread.",
                   fnName);
    return false;
}

// Main-thread + project guard shared by the read callbacks: the live project, or null if called off the
// main thread or before a project exists (the caller then returns its own default).
ScriptProject *guardedProject(void *ctx, const char *fnName) {
    if (!checkMainThread(ctx, fnName))
        return nullptr;
    return pc(ctx)->project;
}

// Decode a set* callback's JSON payload: empty/null text → JSON null (an erase), valid JSON → its value,
// both written into `out` and reported as true. Malformed JSON logs under `fnName`/`key` and returns
// false so the caller drops the call.
bool parsePluginJson(const char *fnName, const char *pluginName, const char *key, const char *jsonUtf8,
                     nlohmann::json &out) {
    out = nlohmann::json{};
    if (jsonUtf8 && jsonUtf8[0] != '\0') {
        out = nlohmann::json::parse(jsonUtf8, nullptr, /*allow_exceptions=*/false);
        if (out.is_discarded()) {
            OFS_CORE_WARN("[Plugin] '{}' {}('{}'): invalid JSON, ignored.", pluginName, fnName, key);
            return false;
        }
    }
    return true;
}

double hostGetTime(void *ctx) {
    if (!checkMainThread(ctx, "getTime"))
        return 0.0;
    auto *p = pc(ctx);
    return p->project ? p->project->playback.cursorPos : 0.0;
}

double hostGetDuration(void *ctx) {
    if (!checkMainThread(ctx, "getDuration"))
        return 0.0;
    auto *p = pc(ctx);
    return p->player ? p->player->getDuration() : 0.0;
}

int hostIsPlaying(void *ctx) {
    if (!checkMainThread(ctx, "isPlaying"))
        return 0;
    auto *p = pc(ctx);
    return p->player && !p->player->isPaused() ? 1 : 0;
}

float hostGetSpeed(void *ctx) {
    if (!checkMainThread(ctx, "getSpeed"))
        return 1.0f;
    auto *p = pc(ctx);
    return p->player ? p->player->getPlaybackSpeed() : 1.0f;
}

int hostGetMediaPath(void *ctx, char *buf, int bufSize) {
    if (!checkMainThread(ctx, "getMediaPath"))
        return emptyBuf(buf, bufSize);
    auto *p = pc(ctx);
    if (!p->project)
        return emptyBuf(buf, bufSize);
    return fillBuf(buf, bufSize, p->project->state.mediaPath);
}

float hostGetVolume(void *ctx) {
    if (!checkMainThread(ctx, "getVolume"))
        return 0.0f;
    auto *p = pc(ctx);
    return p->player ? p->player->getVolume() : 0.0f;
}

void hostSetVolume(void *ctx, float volume) {
    if (!checkMainThread(ctx, "setVolume"))
        return;
    auto *p = pc(ctx);
    if (p->eventQueue)
        p->eventQueue->push(VolumeChangedEvent{volume});
}

double hostGetFps(void *ctx) {
    if (!checkMainThread(ctx, "getFps"))
        return 0.0;
    auto *p = pc(ctx);
    return p->player ? p->player->getFps() : 0.0;
}

int hostGetVideoWidth(void *ctx) {
    if (!checkMainThread(ctx, "getVideoWidth"))
        return 0;
    auto *p = pc(ctx);
    return p->player ? p->player->getWidth() : 0;
}

int hostGetVideoHeight(void *ctx) {
    if (!checkMainThread(ctx, "getVideoHeight"))
        return 0;
    auto *p = pc(ctx);
    return p->player ? p->player->getHeight() : 0;
}

double hostGetMoveStepTime(void *ctx) {
    if (!checkMainThread(ctx, "getMoveStepTime"))
        return 0.0;
    auto *p = pc(ctx);
    return p->project ? stepTime(p->project->overlay) : 0.0;
}

int hostGetAxisRoles(void *ctx, int *rolesBuf, int bufSize) {
    if (!checkMainThread(ctx, "getAxisRoles"))
        return 0;
    auto *p = pc(ctx);
    if (!p->project)
        return 0;
    int count = 0;
    for (size_t i = 0; i < kStandardAxisCount; ++i)
        if (axisExists(*p->project, i))
            ++count;
    if (rolesBuf) {
        int filled = 0;
        for (size_t i = 0; i < kStandardAxisCount && filled < bufSize; ++i) {
            if (axisExists(*p->project, i))
                rolesBuf[filled++] = static_cast<int>(i);
        }
    }
    return count;
}

int hostGetActiveAxisRole(void *ctx) {
    // Off the main thread this is misuse — return -1 like the other axis getters' guard (the managed
    // wrapper asserts the main thread before ever calling, so a plugin never sees it). On the main thread
    // there is always an active axis (it defaults to L0, which can't be removed or hidden), so clamp any
    // degenerate value (no project, or the host's >= Count "none" sentinel) to L0: the API contracts
    // Axes.Active as never-none.
    if (!checkMainThread(ctx, "getActiveAxisRole"))
        return -1;
    auto *p = pc(ctx);
    if (!p->project)
        return 0;
    const int role = static_cast<int>(p->project->state.activeAxis);
    return (role >= 0 && role < static_cast<int>(kStandardAxisCount)) ? role : 0;
}

int hostGetAxisActions(void *ctx, int role, PluginAction *buf, int bufSize) {
    if (!checkMainThread(ctx, "getAxisActions"))
        return 0;
    const AxisState *axis = validAxis(pc(ctx), role);
    return axis ? copyActionsToBuffer(axis->actions, buf, bufSize) : 0;
}

int hostGetAxisActionCount(void *ctx, int role) {
    if (!checkMainThread(ctx, "getAxisActionCount"))
        return 0;
    const AxisState *axis = validAxis(pc(ctx), role);
    return axis ? static_cast<int>(axis->actions.size()) : 0;
}

void hostCommitAxisActions(void *ctx, int role, const PluginAction *actions, int count) {
    if (!checkMainThread(ctx, "commitAxisActions"))
        return;
    auto *p = pc(ctx);
    if (!p->eventQueue || !validAxis(p, role))
        return;
    const auto axisRole = static_cast<StandardAxis>(role);
    auto view = std::span(actions, count) |
                std::views::transform([](const PluginAction &a) { return ScriptAxisAction{a.at, a.pos}; });
    p->eventQueue->push(
        CommitAxisActionsEvent{.axis = axisRole, .actions = VectorSet<ScriptAxisAction>(view.begin(), view.end())});
}

int hostGetAxisSelection(void *ctx, int role, PluginAction *buf, int bufSize) {
    if (!checkMainThread(ctx, "getAxisSelection"))
        return 0;
    const AxisState *axis = validAxis(pc(ctx), role);
    return axis ? copyActionsToBuffer(axis->selection, buf, bufSize) : 0;
}

int hostGetAxisSelectionCount(void *ctx, int role) {
    if (!checkMainThread(ctx, "getAxisSelectionCount"))
        return 0;
    const AxisState *axis = validAxis(pc(ctx), role);
    return axis ? static_cast<int>(axis->selection.size()) : 0;
}

void hostSetAxisSelection(void *ctx, int role, const PluginAction *actions, int count) {
    if (!checkMainThread(ctx, "setAxisSelection"))
        return;
    auto *p = pc(ctx);
    if (!p->eventQueue || !validAxis(p, role))
        return;
    auto view = std::span(actions, count) |
                std::views::transform([](const PluginAction &a) { return ScriptAxisAction{a.at, a.pos}; });
    // Deferred, not applied inline: a buffered AxisEdit commits its actions then its selection, and the
    // actions ride CommitAxisActionsEvent through the queue. Pushing the selection too keeps them ordered,
    // so the selection filters against the just-committed actions instead of the stale set (which would
    // drop any newly-created points). See SetAxisSelectionEvent.
    p->eventQueue->push(SetAxisSelectionEvent{.axis = static_cast<StandardAxis>(role),
                                              .selection = VectorSet<ScriptAxisAction>(view.begin(), view.end())});
}

void hostClearAxisSelection(void *ctx, int role) {
    if (!checkMainThread(ctx, "clearAxisSelection"))
        return;
    auto *p = pc(ctx);
    if (!p->project || !p->eventQueue || role < 0 || role >= static_cast<int>(kStandardAxisCount))
        return;
    p->eventQueue->push(SetAxisSelectionEvent{.axis = static_cast<StandardAxis>(role), .selection = {}});
}

void hostSeekTo(void *ctx, double time) {
    if (!checkMainThread(ctx, "seekTo"))
        return;
    auto *p = pc(ctx);
    if (p->eventQueue)
        p->eventQueue->push(SeekEvent{time});
}

void hostSetPlaying(void *ctx, int playing) {
    if (!checkMainThread(ctx, "setPlaying"))
        return;
    auto *p = pc(ctx);
    if (!p->eventQueue || !p->player)
        return;
    bool wantPlaying = playing != 0;
    if (p->player->isPaused() == wantPlaying)
        p->eventQueue->push(PlayPauseEvent{});
}

void hostSetSpeed(void *ctx, float speed) {
    if (!checkMainThread(ctx, "setSpeed"))
        return;
    auto *p = pc(ctx);
    if (p->eventQueue)
        p->eventQueue->push(PlaybackSpeedEvent{speed});
}

int hostRegisterCommand(void *ctx, const OfsCommandDef *def) {
    if (!checkMainThread(ctx, "registerCommand"))
        return OfsRegisterCommandErrWrongThread;
    if (!checkOnLoad(ctx, "registerCommand"))
        return OfsRegisterCommandErrNotOnLoad;
    auto *p = pc(ctx);
    if (!p->currentPluginName || !def || !def->id || !def->title || def->id[0] == '\0' || def->title[0] == '\0')
        return OfsRegisterCommandErrInvalidArg;
    // A command listed on no surface (neither the rebind UI nor the palette) can never be discovered to
    // bind or invoke — reject it.
    if (def->inRebindList == 0 && def->inPalette == 0) {
        OFS_CORE_ERROR("[Plugin] '{}' tried to register command '{}' that is listed in neither the rebind UI "
                       "nor the palette.",
                       p->currentPluginName, def->id);
        return OfsRegisterCommandErrNotInvokable;
    }
    std::string fullId = fmt::format("{}.{}", p->currentPluginName, def->id);
    if (p->commandRegistry && p->commandRegistry->find(fullId)) {
        OFS_CORE_ERROR("[Plugin] '{}' tried to register already-registered command id '{}'.", p->currentPluginName,
                       fullId);
        return OfsRegisterCommandErrDuplicateId;
    }
    std::string pluginName = p->currentPluginName;
    std::string localId = def->id;
    PluginManager *mgr = p->manager;
    Command cmd;
    cmd.id = fullId;
    cmd.group = pluginName;              // always plugin name for removeByGroup cleanup on unload
    cmd.title = std::string(def->title); // plugin titles carry no catalog key — store the raw UTF-8 string
    cmd.source = CommandSource::Plugin;
    cmd.inRebindList = def->inRebindList != 0;
    cmd.inPalette = def->inPalette != 0;
    cmd.run = [mgr, pluginName, localId](EventQueue &) {
        if (mgr)
            mgr->firePluginCommand(pluginName, localId);
    };
    if (p->commandRegistry)
        p->commandRegistry->add(std::move(cmd));
    return OfsRegisterCommandOk;
}

// True when widgets should lay into the plugin's 2-column Section() form table: inside a uiPushSection
// scope. Tracked via the section depth (not ImGui::GetCurrentTable()) so a host-managed widget that
// opens its own table internally is never mistaken for the form table.
bool inFormSection(PluginCtx *p) {
    return p->uiSectionDepth > 0;
}

void pluginFormRow(const char *label) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    // Left-aligned: the label column auto-fits to the widest label (see hostUiPushSection), so a plugin
    // can use labels of any length without clipping — no hardcoded label width.
    ImGui::TextUnformatted(label);
    ImGui::TableNextColumn();
}

// Places the next plugin widget and returns the label to hand the ImGui call. Three placements:
//   - inside a uiPushRow: flow on one line (SameLine after the first); width-bearing widgets split
//     rowAvailW evenly; every widget renders its OWN label.
//   - inside a uiPushSection form table: a 2-column row. ownLabel widgets (button/radio) paint their
//     label across the value column; others get a left label cell + a hidden "##v" widget label.
//   - bare (no section/row): the widget keeps its own label, full width if width-bearing.
// `fillWidth` = the widget honors SetNextItemWidth (slider/combo/input/drag). `ownLabel` = the widget
// draws its own label (button/radio/label), not a separate left-column label. Pairs with endField().
const char *beginField(PluginCtx *p, const char *label, bool fillWidth, bool ownLabel) {
    ImGui::PushID(p->uiIdCounter++);
    if (p->inRow) {
        if (p->rowItemIndex > 0)
            ImGui::SameLine();
        ++p->rowItemIndex;
        if (fillWidth) {
            auto n = static_cast<float>(p->rowItemCount > 0 ? p->rowItemCount : 1);
            float w = (p->rowAvailW - ImGui::GetStyle().ItemSpacing.x * (n - 1.f)) / n;
            ImGui::SetNextItemWidth(w > 1.f ? w : 1.f);
        }
        return label;
    }
    if (inFormSection(p)) {
        if (ownLabel) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TableNextColumn();
        } else {
            pluginFormRow(label);
        }
        if (fillWidth)
            ImGui::SetNextItemWidth(-FLT_MIN);
        return ownLabel ? label : "##v";
    }
    // Bare (no section/row). A non-ownLabel widget (combo/slider/input/drag) draws its label to the RIGHT
    // of the control, so filling the full width (-FLT_MIN) shoves the label off the edge and it gets
    // clipped. Reserve room for the visible label instead — a negative item width fills to the right edge
    // minus that reserve. An ownLabel widget (button) paints its label inside the frame, so it can take the
    // whole width.
    if (fillWidth) {
        float reserve = 0.f;
        if (!ownLabel && label && label[0] != '\0') {
            const float lw = ImGui::CalcTextSize(label, nullptr, /*hide_text_after_double_hash*/ true).x;
            if (lw > 0.f)
                reserve = lw + ImGui::GetStyle().ItemInnerSpacing.x;
        }
        ImGui::SetNextItemWidth(reserve > 0.f ? -reserve : -FLT_MIN);
    }
    return label;
}

void endField(PluginCtx *p) {
    // Inside a uiPushDisabledTooltip scope (disabled), surface the explanatory tooltip on hover of this
    // greyed widget. SetItemTooltip won't fire on a disabled item, so test hover explicitly with
    // AllowWhenDisabled. Empty outside such a scope → the common path is a single string check.
    if (!p->disabledTooltip.empty() &&
        ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled) &&
        ImGui::BeginTooltip()) {
        // These are sentence-length disclaimers; wrap them (font-relative, not a literal px) so a long or
        // translated string doesn't stretch into one very wide line.
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 25.f);
        ImGui::TextUnformatted(p->disabledTooltip.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
    ImGui::PopID();
}

// Shared body of every value-returning ui* widget: main-thread guard, the beginField label/layout setup,
// the widget call (supplied by `body`, which reports whether it changed), then endField. `body` receives
// the resolved label and returns the widget's changed/clicked bool. Callers with a pre-beginField guard
// (a null buffer/pointer that must skip the field entirely) stay hand-written.
template <typename Fn>
int uiField(void *ctx, const char *fnName, const char *label, bool fillWidth, bool ownLabel, Fn body) {
    if (!checkMainThread(ctx, fnName))
        return 0;
    const char *l = beginField(pc(ctx), label, fillWidth, ownLabel);
    const bool changed = body(l);
    endField(pc(ctx));
    return changed ? 1 : 0;
}

void hostUiLabel(void *ctx, const char *text) {
    if (!checkMainThread(ctx, "uiLabel"))
        return;
    const char *l = beginField(pc(ctx), text, /*fillWidth*/ false, /*ownLabel*/ true);
    ImGui::TextUnformatted(l);
    endField(pc(ctx));
}

void hostUiTextWrapped(void *ctx, const char *text) {
    if (!checkMainThread(ctx, "uiTextWrapped"))
        return;
    const char *l = beginField(pc(ctx), text, /*fillWidth*/ false, /*ownLabel*/ true);
    ImGui::TextWrapped("%s", l); // wraps to the cell width in a section, else the window content width
    endField(pc(ctx));
}

int hostUiButton(void *ctx, const char *label) {
    return uiField(ctx, "uiButton", label, /*fillWidth*/ true, /*ownLabel*/ true,
                   [&](const char *l) { return ImGui::Button(l); });
}

// Builds a bounded "%.Nf" display format from a plugin-supplied decimal count. A plugin only ever
// chooses N (clamped to [0,9]); it never hands us a raw printf format string — that would be a
// format-string vulnerability across the C ABI boundary.
void floatFormat(char (&out)[8], int decimals) {
    std::snprintf(out, sizeof(out), "%%.%df", std::clamp(decimals, 0, 9));
}

int hostUiSliderFloat(void *ctx, const char *label, float *value, float min, float max, int decimals) {
    return uiField(ctx, "uiSliderFloat", label, /*fillWidth*/ true, /*ownLabel*/ false, [&](const char *l) {
        char fmt[8];
        floatFormat(fmt, decimals);
        return ImGui::SliderFloat(l, value, min, max, fmt);
    });
}

int hostUiSliderInt(void *ctx, const char *label, int *value, int min, int max) {
    return uiField(ctx, "uiSliderInt", label, /*fillWidth*/ true, /*ownLabel*/ false,
                   [&](const char *l) { return ImGui::SliderInt(l, value, min, max); });
}

int hostUiCombo(void *ctx, const char *label, int *index, const char *itemsSeparatedByZeros) {
    return uiField(ctx, "uiCombo", label, /*fillWidth*/ true, /*ownLabel*/ false,
                   [&](const char *l) { return ImGui::Combo(l, index, itemsSeparatedByZeros); });
}

int hostUiCheckbox(void *ctx, const char *label, int *value) {
    return uiField(ctx, "uiCheckbox", label, /*fillWidth*/ false, /*ownLabel*/ false, [&](const char *l) {
        bool bval = *value != 0;
        bool changed = ImGui::Checkbox(l, &bval);
        if (changed)
            *value = bval ? 1 : 0;
        return changed;
    });
}

int hostUiRadioButton(void *ctx, const char *label, int *value, int option) {
    return uiField(ctx, "uiRadioButton", label, /*fillWidth*/ false, /*ownLabel*/ true,
                   [&](const char *l) { return ImGui::RadioButton(l, value, option); });
}

int hostUiInputInt(void *ctx, const char *label, int *value, int step) {
    return uiField(ctx, "uiInputInt", label, /*fillWidth*/ true, /*ownLabel*/ false,
                   [&](const char *l) { return ImGui::InputInt(l, value, step); });
}

int hostUiInputFloat(void *ctx, const char *label, float *value, float step, int decimals) {
    return uiField(ctx, "uiInputFloat", label, /*fillWidth*/ true, /*ownLabel*/ false, [&](const char *l) {
        char fmt[8];
        floatFormat(fmt, decimals);
        return ImGui::InputFloat(l, value, step, 0.f, fmt);
    });
}

int hostUiDragInt(void *ctx, const char *label, int *value, float speed, int min, int max) {
    return uiField(ctx, "uiDragInt", label, /*fillWidth*/ true, /*ownLabel*/ false,
                   [&](const char *l) { return ImGui::DragInt(l, value, speed, min, max); });
}

int hostUiDragFloat(void *ctx, const char *label, float *value, float speed, float min, float max, int decimals) {
    return uiField(ctx, "uiDragFloat", label, /*fillWidth*/ true, /*ownLabel*/ false, [&](const char *l) {
        char fmt[8];
        floatFormat(fmt, decimals);
        return ImGui::DragFloat(l, value, speed, min, max, fmt);
    });
}

int hostUiInputText(void *ctx, const char *label, char *buf, int bufSize, int password, int readOnly) {
    if (!checkMainThread(ctx, "uiInputText"))
        return 0;
    if (!buf || bufSize <= 0)
        return 0;
    const char *l = beginField(pc(ctx), label, /*fillWidth*/ true, /*ownLabel*/ false);
    ImGuiInputTextFlags flags = 0;
    if (password)
        flags |= ImGuiInputTextFlags_Password;
    if (readOnly)
        flags |= ImGuiInputTextFlags_ReadOnly;
    bool changed = ImGui::InputText(l, buf, static_cast<size_t>(bufSize), flags);
    endField(pc(ctx));
    return changed ? 1 : 0;
}

void hostUiSeparator(void *ctx) {
    if (!checkMainThread(ctx, "uiSeparator"))
        return;
    ImGui::Separator();
}

// Stable per-row key into the window's ImGui state storage, used to remember the row's widget count
// across frames (the even-split divisor). Valid only while the row's PushID scope is active.
ImGuiID rowCountKey() {
    return ImGui::GetID("##ofs_row_count");
}

void hostUiPushRow(void *ctx, const char *label) {
    if (!checkMainThread(ctx, "uiPushRow"))
        return;
    auto *p = pc(ctx);
    if (p->inRow) // rows don't nest; ignore the inner push so the PushID/PopID pair stays balanced
        return;
    p->inRow = true;
    p->rowItemIndex = 0;
    ImGui::PushID(p->uiIdCounter++);
    p->rowItemCount = std::max(1, ImGui::GetStateStorage()->GetInt(rowCountKey(), 1));
    if (inFormSection(p)) {
        pluginFormRow(label);
    } else if (label && label[0]) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine();
    }
    p->rowAvailW = ImGui::GetContentRegionAvail().x;
}

void hostUiPopRow(void *ctx) {
    if (!checkMainThread(ctx, "uiPopRow"))
        return;
    auto *p = pc(ctx);
    if (!p->inRow)
        return;
    p->inRow = false;
    ImGui::GetStateStorage()->SetInt(rowCountKey(), p->rowItemIndex);
    ImGui::PopID();
}

void hostUiPushSection(void *ctx, const char *title) {
    if (!checkMainThread(ctx, "uiPushSection"))
        return;
    auto *p = pc(ctx);
    if (p->uiSectionDepth == 0) {
        // Outermost section owns the 2-column form table that inFormSection()/pluginFormRow render into.
        ImGui::SeparatorText(title);
        ImGui::PushID(p->uiIdCounter++);
        float w = ImGui::GetContentRegionAvail().x - ofs::ui::kRightGap;
        ImGui::BeginTable(fmtScratch("##sec{}", p->uiIdCounter), 2, ImGuiTableFlags_None, {w, 0.f});
        // WidthFixed with no init width: the label column auto-fits to its widest label cell, so plugin
        // labels of any length stay readable; the value column stretches into the remaining width.
        ImGui::TableSetupColumn("##L", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("##R", ImGuiTableColumnFlags_WidthStretch);
    } else {
        // Nested section: a second BeginTable here would be outside any cell and assert ("TableSetupColumn
        // called too many times"). Reuse the outer table instead — emit the heading as its own row so the
        // nesting flattens visually but the table+ID stack stays balanced.
        ImGui::PushID(p->uiIdCounter++);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::SeparatorText(title);
    }
    ++p->uiSectionDepth;
}

void hostUiPopSection(void *ctx) {
    if (!checkMainThread(ctx, "uiPopSection"))
        return;
    auto *p = pc(ctx);
    // Defensive: a malformed plugin could pop more than it pushed; never unwind ImGui past the table.
    if (p->uiSectionDepth == 0)
        return;
    --p->uiSectionDepth;
    if (p->uiSectionDepth == 0)
        ImGui::EndTable();
    ImGui::PopID();
}

void hostUiPushDisabled(void *ctx, int disabled) {
    if (!checkMainThread(ctx, "uiPushDisabled"))
        return;
    auto *p = pc(ctx);
    ImGui::BeginDisabled(disabled != 0); // disabled==0 still pushes a level so push/pop stays balanced
    ++p->uiDisabledDepth;
}

void hostUiPopDisabled(void *ctx) {
    if (!checkMainThread(ctx, "uiPopDisabled"))
        return;
    auto *p = pc(ctx);
    if (p->uiDisabledDepth == 0) // defensive: never EndDisabled past our own BeginDisabled calls
        return;
    --p->uiDisabledDepth;
    ImGui::EndDisabled();
}

void hostUiTooltip(void *ctx, const char *text) {
    if (!checkMainThread(ctx, "uiTooltip"))
        return;
    if (text && text[0])
        ImGui::SetItemTooltip("%s", text); // item-bound: a safe no-op if no widget precedes it
}

void hostUiPushDisabledTooltip(void *ctx, int disabled, const char *tooltip) {
    if (!checkMainThread(ctx, "uiPushDisabledTooltip"))
        return;
    auto *p = pc(ctx);
    ImGui::BeginDisabled(disabled != 0);
    ++p->uiDisabledDepth;
    // endField shows this on hover of each greyed widget; only arm it while actually disabled. Save the
    // enclosing scope's text so a nested disabled-tooltip scope restores it on pop.
    p->disabledTooltipStack.push_back(p->disabledTooltip);
    p->disabledTooltip = (disabled != 0 && tooltip) ? tooltip : std::string();
}

void hostUiPopDisabledTooltip(void *ctx) {
    if (!checkMainThread(ctx, "uiPopDisabledTooltip"))
        return;
    auto *p = pc(ctx);
    if (p->uiDisabledDepth == 0) // defensive: stay balanced with our own BeginDisabled
        return;
    --p->uiDisabledDepth;
    ImGui::EndDisabled();
    if (!p->disabledTooltipStack.empty()) {
        p->disabledTooltip = std::move(p->disabledTooltipStack.back());
        p->disabledTooltipStack.pop_back();
    } else {
        p->disabledTooltip.clear();
    }
}

void hostRegisterNode(void *ctx, const OfsNodeDef *def) {
    if (!checkMainThread(ctx, "registerNode"))
        return;
    if (!checkOnLoad(ctx, "registerNode"))
        return;
    auto *p = pc(ctx);
    if (!p->eventQueue || !def || !def->id || !def->displayName || !p->currentPluginName)
        return;

    // Pin counts are bounded by the GraphId slot field (OFS_MAX_NODE_PINS). A def outside the range is a
    // plugin bug; reject it rather than mint pins the id encoding can't represent. Every output node needs
    // at least one output.
    if (def->inputCount < 0 || def->inputCount > OFS_MAX_NODE_PINS || def->outputCount < 1 ||
        def->outputCount > OFS_MAX_NODE_PINS) {
        OFS_CORE_WARN("[Plugin] '{}' registerNode '{}': pin counts out of range (in={}, out={})", p->currentPluginName,
                      def->id, def->inputCount, def->outputCount);
        return;
    }

    PluginNodeEntry entry;
    entry.id = fmt::format("{}.{}", p->currentPluginName, def->id);
    entry.displayName = def->displayName;
    // An author-declared group overrides the default plugin-name bucket (rendered verbatim, no catalog
    // localization); the icon is a curated enum value the UI maps to a glyph (Default → arity bucket).
    entry.category = (def->group && def->group[0]) ? def->group : p->currentPluginName;
    entry.description = (def->description && def->description[0]) ? def->description : "";
    entry.icon = def->icon;
    entry.signal = static_cast<OfsSignalKind>(def->signal);
    entry.inputCount = def->inputCount;
    entry.outputCount = def->outputCount;
    entry.fn = def->fn;
    entry.userData = def->userData;
    entry.onNodeUi = def->onNodeUi;
    entry.hasState = def->hasState != 0;
    // A TState node encodes its managed slot index in userData (the same index the trampolines and the
    // capture codec key off). Carried here so the snapshot-build capture can name the codec closure.
    entry.slot = static_cast<int>(reinterpret_cast<intptr_t>(def->userData));

    // Copy the per-pin names verbatim (host-owned strings; not serialized — a disabled plugin falls back
    // to index labels from the persisted counts). A null name pointer degrades to an empty label.
    entry.inputNames.reserve(static_cast<size_t>(def->inputCount));
    for (int i = 0; i < def->inputCount; ++i)
        entry.inputNames.emplace_back((def->inputNames && def->inputNames[i]) ? def->inputNames[i] : "");
    entry.outputNames.reserve(static_cast<size_t>(def->outputCount));
    for (int i = 0; i < def->outputCount; ++i)
        entry.outputNames.emplace_back((def->outputNames && def->outputNames[i]) ? def->outputNames[i] : "");

    p->eventQueue->push(RegisterPluginNodeEvent{std::move(entry)});
}

// Publish an edit mode the user can pick in the footer. Main-thread, onLoad-only, like registerNode.
// The router is the sole subscriber to RegisterEditModeEvent; it owns the registry. The fn pointers are
// the plugin's C-ABI trampolines and `userData` the opaque slot the managed wrapper keys off — both
// stay valid until the plugin unloads, when the matching UnregisterEditModesEvent drops the entry.
void hostRegisterEditMode(void *ctx, const OfsEditModeDef *def) {
    if (!checkMainThread(ctx, "registerEditMode"))
        return;
    if (!checkOnLoad(ctx, "registerEditMode"))
        return;
    auto *p = pc(ctx);
    if (!p->eventQueue || !def || !def->id || !p->currentPluginName)
        return;
    EditModeEntry entry;
    entry.id = fmt::format("{}.{}", p->currentPluginName, def->id);
    entry.displayName = def->displayName ? def->displayName : "";
    entry.owningPlugin = p->currentPluginName;
    entry.onEditIntent = def->onEditIntent;
    entry.onEnter = def->onEnter;
    entry.onExit = def->onExit;
    entry.onUi = def->onUi;
    entry.userData = def->userData;
    p->eventQueue->push(RegisterEditModeEvent{std::move(entry)});
}

// Publish a navigator the user can pick in the footer. Mirrors hostRegisterEditMode; NavigatorRouter is
// the sole subscriber to RegisterNavigatorEvent.
void hostRegisterNavigator(void *ctx, const OfsNavigatorDef *def) {
    if (!checkMainThread(ctx, "registerNavigator"))
        return;
    if (!checkOnLoad(ctx, "registerNavigator"))
        return;
    auto *p = pc(ctx);
    if (!p->eventQueue || !def || !def->id || !p->currentPluginName)
        return;
    NavigatorEntry entry;
    entry.id = fmt::format("{}.{}", p->currentPluginName, def->id);
    entry.displayName = def->displayName ? def->displayName : "";
    entry.owningPlugin = p->currentPluginName;
    entry.onStep = def->onStep;
    entry.onEnter = def->onEnter;
    entry.onExit = def->onExit;
    entry.onUi = def->onUi;
    entry.userData = def->userData;
    p->eventQueue->push(RegisterNavigatorEvent{std::move(entry)});
}

// 1 if the calling plugin's navigator `localId` is the active one. Mirrors hostIsEditModeActive.
int hostIsNavigatorActive(void *ctx, const char *localId) {
    if (!checkMainThread(ctx, "isNavigatorActive"))
        return 0;
    auto *p = pc(ctx);
    if (!p->project || !p->currentPluginName || !localId || !localId[0])
        return 0;
    return p->project->activeNavigator == fmt::format("{}.{}", p->currentPluginName, localId) ? 1 : 0;
}

// 1 if the calling plugin's edit mode `localId` is the active selection. Resolves the full id the same
// way hostRegisterEditMode does (plugin name + "." + localId), so a plugin can gate its UI on its mode
// being selected without knowing the namespaced id the host assigned it.
int hostIsEditModeActive(void *ctx, const char *localId) {
    if (!checkMainThread(ctx, "isEditModeActive"))
        return 0;
    auto *p = pc(ctx);
    if (!p->project || !p->currentPluginName || !localId || !localId[0])
        return 0;
    return p->project->activeEditMode == fmt::format("{}.{}", p->currentPluginName, localId) ? 1 : 0;
}

// Publish a selection mode the user can pick in the footer's Select selector. Mirrors hostRegisterEditMode;
// SelectIntentRouter is the sole subscriber to RegisterSelectionModeEvent. The onSelect/onEnter/onExit
// trampolines + userData stay valid until the plugin unloads.
void hostRegisterSelectionMode(void *ctx, const OfsSelectModeDef *def) {
    if (!checkMainThread(ctx, "registerSelectionMode"))
        return;
    if (!checkOnLoad(ctx, "registerSelectionMode"))
        return;
    auto *p = pc(ctx);
    if (!p->eventQueue || !def || !def->id || !p->currentPluginName)
        return;
    SelectionModeEntry entry;
    entry.id = fmt::format("{}.{}", p->currentPluginName, def->id);
    entry.displayName = def->displayName ? def->displayName : "";
    entry.owningPlugin = p->currentPluginName;
    entry.onSelect = def->onSelect;
    entry.onEnter = def->onEnter;
    entry.onExit = def->onExit;
    entry.onUi = def->onUi;
    entry.userData = def->userData;
    p->eventQueue->push(RegisterSelectionModeEvent{std::move(entry)});
}

// 1 if the calling plugin's selection mode `localId` is the active one. Mirrors hostIsEditModeActive.
int hostIsSelectionModeActive(void *ctx, const char *localId) {
    if (!checkMainThread(ctx, "isSelectionModeActive"))
        return 0;
    auto *p = pc(ctx);
    if (!p->project || !p->currentPluginName || !localId || !localId[0])
        return 0;
    return p->project->activeSelectionMode == fmt::format("{}.{}", p->currentPluginName, localId) ? 1 : 0;
}

// ── Plugin-node custom UI state ───────────────────────────────────────────────
// Valid only inside an onNodeUi call, when the node-body renderer has set the current node on the ctx
// (phase 4e). Outside that window currentNodeState is null and these are inert.
const char *hostNodeUiGetState(void *ctx) {
    if (!checkMainThread(ctx, "nodeUiGetState"))
        return "";
    auto *p = pc(ctx);
    return (p && p->currentNodeState) ? p->currentNodeState->c_str() : "";
}

void hostNodeUiSetState(void *ctx, const char *stateJsonUtf8) {
    if (!checkMainThread(ctx, "nodeUiSetState"))
        return;
    auto *p = pc(ctx);
    if (!p || !p->currentNodeState || !stateJsonUtf8)
        return;
    p->currentNodeState->assign(stateJsonUtf8);
    p->currentNodeStateChanged = true; // the renderer turns this into a ModifyRegionEvent (phase 4e)
}

// Packed (regionId << 32) | (uint32)nodeId of the current onNodeUi node; -1 outside such a call. The
// managed Node<TState> handle stores this so a deferred Node.Update can target the right node.
long long hostNodeUiCurrentKey(void *ctx) {
    if (!checkMainThread(ctx, "nodeUiCurrentKey"))
        return -1;
    auto *p = pc(ctx);
    if (!p || p->currentNodeRegionId < 0 || p->currentNodeId < 0)
        return -1;
    return (static_cast<long long>(p->currentNodeRegionId) << 32) |
           static_cast<long long>(static_cast<unsigned int>(p->currentNodeId));
}

void hostLog(void *ctx, int level, const char *msg) {
    if (!msg)
        return;
    auto *p = pc(ctx);
    const char *who = (p && p->currentPluginName) ? p->currentPluginName : "plugin";
    // level matches the managed LogLevel enum. No core debug macro exists, so trace/debug share trace.
    switch (level) {
    case 0:
    case 1:
        OFS_CORE_TRACE("[{}] {}", who, msg);
        break;
    case 3:
        OFS_CORE_WARN("[{}] {}", who, msg);
        break;
    case 4:
        OFS_CORE_ERROR("[{}] {}", who, msg);
        break;
    default:
        OFS_CORE_INFO("[{}] {}", who, msg);
        break;
    }
}

void hostReportFault(void *ctx, const char *faultCtx) {
    auto *p = pc(ctx);
    if (!p || !p->manager || !faultCtx)
        return;
    // `faultCtx` is the faulting entry point (e.g. "OnUpdate", "node:gen"); the plugin name comes from
    // the active call context (null on worker threads, where node faults are attributed generically).
    const char *who = p->currentPluginName ? p->currentPluginName : "plugin";
    p->manager->notifyPluginFault(who, faultCtx);
}

void hostNotify(void *ctx, int level, const char *msg) {
    auto *p = pc(ctx);
    if (!p || !p->manager || !msg)
        return;
    const char *who = p->currentPluginName ? p->currentPluginName : "plugin";
    // Clamp an out-of-range level to Info so a bad value can never index past the enum.
    const auto nl = (level >= 0 && level <= static_cast<int>(NotifyLevel::Error)) ? static_cast<NotifyLevel>(level)
                                                                                  : NotifyLevel::Info;
    p->manager->notifyPlugin(who, nl, msg);
}

// ── Shared buffer/string helpers for the read-back getters below ─────────────
// Copy a UTF-8 string into a plugin-provided buffer, truncated on a codepoint boundary to bufSize-1 and
// NUL-terminated. Returns the FULL source byte length (excluding the NUL), NOT the number copied — so a
// caller that passed too small a buffer learns the exact size it needs and can re-call once with a
// buffer of (return + 1) bytes (the managed GrowAndRead contract). A null/zero buffer copies nothing
// but still reports the required length. The return is thus a truncation signal: required >= bufSize.
int fillBuf(char *buf, int bufSize, std::string_view s) {
    int required = static_cast<int>(s.size());
    if (!buf || bufSize <= 0)
        return required;
    int n = utf8ClampLen(s, bufSize - 1);
    if (n > 0)
        std::memcpy(buf, s.data(), static_cast<size_t>(n));
    buf[n] = '\0';
    return required;
}

int emptyBuf(char *buf, int bufSize) {
    return fillBuf(buf, bufSize, {});
}

// Fill a composite getter's name out-buffer and its required-length out-param in one place: clip the name
// to nameBuf, NUL-terminate, and report the full byte length the caller needs (so it can re-read this
// index with a (*nameReqOut + 1) buffer). Safe with a null buffer and/or null nameReqOut. Called once with
// an empty name up front so every early return leaves the out-params cleared.
void fillNameOut(char *nameBuf, int nameBufSize, int *nameReqOut, std::string_view name) {
    int required = fillBuf(nameBuf, nameBufSize, name);
    if (nameReqOut)
        *nameReqOut = required;
}

// Split a ';'-separated glob list ("*.png;*.jpg") into individual patterns. Dialog-only (user-triggered,
// blocking), so the heap allocation here is outside any per-frame hot path.
void parseSemicolonList(const char *s, std::vector<std::string> &out) {
    if (!s)
        return;
    std::string_view sv(s);
    size_t pos = 0;
    while (pos <= sv.size()) {
        size_t sep = sv.find(';', pos);
        size_t len = (sep == std::string_view::npos ? sv.size() : sep) - pos;
        if (len > 0)
            out.emplace_back(sv.substr(pos, len));
        if (sep == std::string_view::npos)
            break;
        pos = sep + 1;
    }
}

// ── Axis & project state ─────────────────────────────────────────────────────

void hostSetActiveAxis(void *ctx, int role) {
    if (!checkMainThread(ctx, "setActiveAxis"))
        return;
    auto *p = pc(ctx);
    if (!p->eventQueue || !validAxis(p, role))
        return;
    p->eventQueue->push(AxisSelectedEvent{static_cast<StandardAxis>(role)});
}

int hostIsAxisVisible(void *ctx, int role) {
    if (!checkMainThread(ctx, "isAxisVisible"))
        return -1;
    const AxisState *a = validAxis(pc(ctx), role);
    return a ? (a->isVisible ? 1 : 0) : -1;
}

int hostIsAxisLocked(void *ctx, int role) {
    if (!checkMainThread(ctx, "isAxisLocked"))
        return -1;
    const AxisState *a = validAxis(pc(ctx), role);
    return a ? (a->isLocked ? 1 : 0) : -1;
}

int hostGetAxisName(void *ctx, int role, char *buf, int bufSize) {
    if (!checkMainThread(ctx, "getAxisName"))
        return emptyBuf(buf, bufSize);
    if (role < 0 || role >= static_cast<int>(kStandardAxisCount))
        return emptyBuf(buf, bufSize);
    return fillBuf(buf, bufSize, standardAxisName(static_cast<StandardAxis>(role)));
}

int hostIsProjectDirty(void *ctx) {
    if (!checkMainThread(ctx, "isProjectDirty"))
        return 0;
    auto *p = pc(ctx);
    if (!p->project)
        return 0;
    // Mirrors ProjectManager::isDirty (settings flag or any dirty axis) without a service call.
    if (p->project->state.settingsDirty)
        return 1;
    return std::ranges::any_of(p->project->axes, [](const AxisState &a) { return a.dirty; }) ? 1 : 0;
}

int hostGetProjectMetadata(void *ctx, char *buf, int bufSize) {
    if (!checkMainThread(ctx, "getProjectMetadata"))
        return emptyBuf(buf, bufSize);
    auto *p = pc(ctx);
    if (!p->project)
        return emptyBuf(buf, bufSize);
    // to_json(FunscriptMetadata) covers all standard fields + tags + performers + custom fields
    nlohmann::json j = p->project->metadata;
    return fillBuf(buf, bufSize, j.dump());
}

int hostGetFunscriptJson(void *ctx, const int *roles, int count, int version, char *buf, int bufSize) {
    if (!checkMainThread(ctx, "getFunscriptJson"))
        return emptyBuf(buf, bufSize);
    auto *p = pc(ctx);
    if (!p->project || !roles || count <= 0)
        return emptyBuf(buf, bufSize);

    // Mirror exportMultiAxisFunscript: collect (funscript tag, actions) per requested role, skipping
    // absent/scratch/empty and any duplicate tag. The host carries the same raw `actions` getAxisActions
    // exposes, so the JSON matches what a plugin reads back action-for-action. 1.0 is single-axis, so stop
    // after the first valid role.
    std::vector<std::pair<std::string, VectorSet<ScriptAxisAction>>> axisData;
    for (int i = 0; i < count; ++i) {
        const AxisState *axis = validAxis(p, roles[i]);
        if (!axis || axis->actions.empty())
            continue;
        const auto role = static_cast<StandardAxis>(roles[i]);
        if (isScratchAxis(role)) // scratch axes (S0–S9) have no funscript tag
            continue;
        std::string tag(standardAxisTag(role));
        if (std::ranges::any_of(axisData, [&](const auto &e) { return e.first == tag; }))
            continue;
        axisData.emplace_back(std::move(tag), axis->actions);
        if (version == OfsFunscript10) // 1.0 is single-axis: the first valid role is the whole document
            break;
    }
    if (axisData.empty())
        return emptyBuf(buf, bufSize);

    Funscript fs;
    switch (version) {
    case OfsFunscript20:
        fs = Funscript::fromAxes20(axisData);
        fs.version = "2.0";
        break;
    case OfsFunscript11:
        fs = Funscript::fromAxes11(axisData);
        fs.version = "1.1";
        break;
    default: // OfsFunscript10 — single axis
        fs = Funscript::fromActions(axisData.front().second);
        fs.version = "1.0";
        break;
    }
    fs.metadata = p->project->metadata;
    return fillBuf(buf, bufSize, nlohmann::json(fs).dump());
}

int hostGetProjectData(void *ctx, const char *key, char *buf, int bufSize) {
    if (!checkMainThread(ctx, "getProjectData"))
        return emptyBuf(buf, bufSize);
    auto *p = pc(ctx);
    if (!p->project || !p->currentPluginName || !key || !p->project->pluginData.is_object())
        return emptyBuf(buf, bufSize);
    auto slot = p->project->pluginData.find(p->currentPluginName);
    if (slot == p->project->pluginData.end() || !slot->is_object())
        return emptyBuf(buf, bufSize);
    auto val = slot->find(key);
    if (val == slot->end() || val->is_null())
        return emptyBuf(buf, bufSize);
    return fillBuf(buf, bufSize, val->dump());
}

void hostSetProjectData(void *ctx, const char *key, const char *jsonUtf8) {
    if (!checkMainThread(ctx, "setProjectData"))
        return;
    auto *p = pc(ctx);
    if (!p->eventQueue || !p->currentPluginName || !key || key[0] == '\0')
        return;
    nlohmann::json value;
    if (!parsePluginJson("setProjectData", p->currentPluginName, key, jsonUtf8, value))
        return;
    p->eventQueue->push(
        SetPluginProjectDataEvent{.pluginName = p->currentPluginName, .key = key, .value = std::move(value)});
}

int hostGetAppData(void *ctx, const char *key, char *buf, int bufSize) {
    if (!checkMainThread(ctx, "getAppData"))
        return emptyBuf(buf, bufSize);
    auto *p = pc(ctx);
    if (!p->manager || !p->currentPluginName || !key)
        return emptyBuf(buf, bufSize);
    const nlohmann::json *val = p->manager->appSettingValue(p->currentPluginName, key);
    if (!val)
        return emptyBuf(buf, bufSize);
    return fillBuf(buf, bufSize, val->dump());
}

void hostSetAppData(void *ctx, const char *key, const char *jsonUtf8) {
    if (!checkMainThread(ctx, "setAppData"))
        return;
    auto *p = pc(ctx);
    if (!p->manager || !p->currentPluginName || !key || key[0] == '\0')
        return;
    nlohmann::json value;
    if (!parsePluginJson("setAppData", p->currentPluginName, key, jsonUtf8, value))
        return;
    p->manager->setAppSetting(p->currentPluginName, key, std::move(value));
}

int hostGetChapterCount(void *ctx) {
    ScriptProject *pr = guardedProject(ctx, "getChapterCount");
    return pr ? static_cast<int>(pr->bookmarks.chapters.size()) : 0;
}

int hostGetChapter(void *ctx, int index, double *startOut, double *endOut, unsigned int *colorOut, char *nameBuf,
                   int nameBufSize, int *nameReqOut) {
    fillNameOut(nameBuf, nameBufSize, nameReqOut, {}); // clear name out-params before any early return
    ScriptProject *pr = guardedProject(ctx, "getChapter");
    const auto *c = pr ? itemAt(pr->bookmarks.chapters, index) : nullptr;
    if (!c)
        return 0;
    if (startOut)
        *startOut = c->startTime;
    if (endOut)
        *endOut = c->endTime;
    if (colorOut)
        *colorOut = c->color;
    fillNameOut(nameBuf, nameBufSize, nameReqOut, c->name);
    return 1;
}

int hostGetBookmarkCount(void *ctx) {
    ScriptProject *pr = guardedProject(ctx, "getBookmarkCount");
    return pr ? static_cast<int>(pr->bookmarks.bookmarks.size()) : 0;
}

int hostGetBookmark(void *ctx, int index, double *timeOut, char *nameBuf, int nameBufSize, int *nameReqOut) {
    fillNameOut(nameBuf, nameBufSize, nameReqOut, {}); // clear name out-params before any early return
    ScriptProject *pr = guardedProject(ctx, "getBookmark");
    const auto *b = pr ? itemAt(pr->bookmarks.bookmarks, index) : nullptr;
    if (!b)
        return 0;
    if (timeOut)
        *timeOut = b->time;
    fillNameOut(nameBuf, nameBufSize, nameReqOut, b->name);
    return 1;
}

int hostGetRegionCount(void *ctx) {
    ScriptProject *pr = guardedProject(ctx, "getRegionCount");
    return pr ? static_cast<int>(pr->regions.size()) : 0;
}

int hostGetRegion(void *ctx, int index, double *startOut, double *endOut, char *nameBuf, int nameBufSize,
                  int *nameReqOut) {
    fillNameOut(nameBuf, nameBufSize, nameReqOut, {}); // clear name out-params before any early return
    ScriptProject *pr = guardedProject(ctx, "getRegion");
    const auto *r = pr ? itemAt(pr->regions, index) : nullptr;
    if (!r)
        return 0;
    if (startOut)
        *startOut = r->startTime;
    if (endOut)
        *endOut = r->endTime;
    fillNameOut(nameBuf, nameBufSize, nameReqOut, r->name);
    return 1;
}

int hostGetActiveLanguage(void *ctx, char *buf, int bufSize) {
    if (!checkMainThread(ctx, "getActiveLanguage"))
        return emptyBuf(buf, bufSize);
    return fillBuf(buf, bufSize, ofs::loc::Translator::instance().activeCulture());
}

// ── Native dialogs (non-blocking) ────────────────────────────────────────────
// Each queues an async modal via the same flow the app's own dialogs use (ShowModalEvent → SDL runs the
// dialog on its own thread, the frame loop keeps rendering), then invokes the plugin's result callback
// once on the main thread when the user answers. The persistence key is the plugin name so each plugin
// keeps its own last-used directory. Under OFS_TEST_ENGINE the modal layer never touches a real OS dialog.

void hostFileDialog(PluginCtx *p, FileDialogSpec spec, OfsFileResultFn onResult, void *userData) {
    spec.key = p->currentPluginName ? p->currentPluginName : "";
    pickFile(*p->eventQueue, std::move(spec), [onResult, userData](const std::string &path) {
        onResult(userData, path.empty() ? nullptr : path.c_str()); // empty == cancelled
    });
}

void hostOpenFileDialog(void *ctx, const char *title, const char *filterPatterns, const char *filterDesc,
                        OfsFileResultFn onResult, void *userData) {
    if (!checkMainThread(ctx, "openFileDialog") || !onResult)
        return;
    FileDialogSpec spec;
    spec.kind = FileDialogKind::Open;
    spec.title = title ? title : "Open";
    parseSemicolonList(filterPatterns, spec.filterPatterns);
    spec.filterDesc = filterDesc ? filterDesc : "";
    hostFileDialog(pc(ctx), std::move(spec), onResult, userData);
}

void hostOpenFilesDialog(void *ctx, const char *title, const char *filterPatterns, const char *filterDesc,
                         OfsFilesResultFn onResult, void *userData) {
    if (!checkMainThread(ctx, "openFilesDialog") || !onResult)
        return;
    FileDialogSpec spec;
    spec.kind = FileDialogKind::Open;
    spec.allowMany = true;
    spec.title = title ? title : "Open";
    parseSemicolonList(filterPatterns, spec.filterPatterns);
    spec.filterDesc = filterDesc ? filterDesc : "";
    auto *p = pc(ctx);
    spec.key = p->currentPluginName ? p->currentPluginName : "";
    pickFiles(*p->eventQueue, std::move(spec), [onResult, userData](const std::vector<std::string> &paths) {
        if (paths.empty()) {
            onResult(userData, nullptr, 0); // cancelled / nothing chosen
            return;
        }
        std::vector<const char *> ptrs;
        ptrs.reserve(paths.size());
        for (const auto &s : paths)
            ptrs.push_back(s.c_str());
        onResult(userData, ptrs.data(), static_cast<int>(ptrs.size()));
    });
}

void hostSaveFileDialog(void *ctx, const char *title, const char *defaultName, const char *filterPatterns,
                        const char *filterDesc, OfsFileResultFn onResult, void *userData) {
    if (!checkMainThread(ctx, "saveFileDialog") || !onResult)
        return;
    FileDialogSpec spec;
    spec.kind = FileDialogKind::Save;
    spec.title = title ? title : "Save";
    spec.defaultName = defaultName ? defaultName : "";
    parseSemicolonList(filterPatterns, spec.filterPatterns);
    spec.filterDesc = filterDesc ? filterDesc : "";
    hostFileDialog(pc(ctx), std::move(spec), onResult, userData);
}

void hostPickFolder(void *ctx, const char *title, OfsFileResultFn onResult, void *userData) {
    if (!checkMainThread(ctx, "pickFolder") || !onResult)
        return;
    FileDialogSpec spec;
    spec.kind = FileDialogKind::SelectFolder;
    spec.title = title ? title : "Select Folder";
    hostFileDialog(pc(ctx), std::move(spec), onResult, userData);
}

void hostConfirmDialog(void *ctx, const char *title, const char *message, int kind, OfsConfirmResultFn onResult,
                       void *userData) {
    if (!checkMainThread(ctx, "confirmDialog") || !onResult)
        return;
    ModalSpec spec;
    spec.title = title ? title : "";
    spec.message = message ? message : "";
    // The affirmative button is index 0 (so a dismissal, which yields -1, maps to the safe negative).
    spec.buttons = kind == 2   ? std::vector<std::string>{"Yes", "No"}
                   : kind == 1 ? std::vector<std::string>{"OK", "Cancel"}
                               : std::vector<std::string>{"OK"};
    confirmAsync(*pc(ctx)->eventQueue, std::move(spec),
                 [onResult, userData](int idx) { onResult(userData, idx == 0 ? 1 : 0); });
}

// ── Immediate-mode UI additions ──────────────────────────────────────────────

int hostUiColorEdit(void *ctx, const char *label, float *rgba, int alpha) {
    if (!checkMainThread(ctx, "uiColorEdit"))
        return 0;
    if (!rgba)
        return 0;
    const char *l = beginField(pc(ctx), label ? label : "", /*fillWidth*/ true, /*ownLabel*/ false);
    // ColorEdit3 reads/writes only rgba[0..2], so the caller's alpha (rgba[3]) is preserved.
    bool changed = alpha ? ImGui::ColorEdit4(l, rgba, ImGuiColorEditFlags_AlphaBar) : ImGui::ColorEdit3(l, rgba);
    endField(pc(ctx));
    return changed ? 1 : 0;
}

int hostUiInputTextMultiline(void *ctx, const char *label, char *buf, int bufSize, int heightLines, int readOnly) {
    if (!checkMainThread(ctx, "uiInputTextMultiline"))
        return 0;
    if (!buf || bufSize <= 0)
        return 0;
    const char *l = beginField(pc(ctx), label ? label : "", /*fillWidth*/ true, /*ownLabel*/ false);
    const float h = heightLines > 0 ? ImGui::GetTextLineHeight() * static_cast<float>(heightLines) +
                                          ImGui::GetStyle().FramePadding.y * 2.f
                                    : 0.f;
    // ImGui's Password flag is single-line only, so multiline exposes read-only alone.
    ImGuiInputTextFlags flags = readOnly ? ImGuiInputTextFlags_ReadOnly : 0;
    bool changed = ImGui::InputTextMultiline(l, buf, static_cast<size_t>(bufSize), ImVec2(0.f, h), flags);
    endField(pc(ctx));
    return changed ? 1 : 0;
}

void hostUiProgressBar(void *ctx, float fraction, const char *overlay) {
    if (!checkMainThread(ctx, "uiProgressBar"))
        return;
    beginField(pc(ctx), "##progress", /*fillWidth*/ false, /*ownLabel*/ true);
    ImGui::ProgressBar(std::clamp(fraction, 0.f, 1.f), ImVec2(-FLT_MIN, 0.f), overlay);
    endField(pc(ctx));
}

} // namespace

PluginManager::PluginManager(ScriptProject &project, EventQueue &eq, std::shared_ptr<VideoPlayer> player,
                             std::shared_ptr<VideoPlayer> dummyPlayer, CommandRegistry &cmdRegistry,
                             BindingSystem &bindingSystem, EffectRegistryState &effectReg)
    : project_(project), eventQueue_(eq), effectReg_(effectReg), player_(std::move(player)),
      dummyPlayer_(std::move(dummyPlayer)) {
    callCtx_.project = &project_;
    callCtx_.eventQueue = &eq;
    callCtx_.player = player_.get();
    callCtx_.manager = this;
    callCtx_.effectReg = &effectReg_;
    callCtx_.commandRegistry = &cmdRegistry;
    callCtx_.bindingSystem = &bindingSystem;
    callCtx_.mainThreadId = std::this_thread::get_id();

    hostApi.version = OFS_ABI_VERSION;
    hostApi.ctx = &callCtx_;
    hostApi.getTime = &hostGetTime;
    hostApi.getDuration = &hostGetDuration;
    hostApi.isPlaying = &hostIsPlaying;
    hostApi.getSpeed = &hostGetSpeed;
    hostApi.getMediaPath = &hostGetMediaPath;
    hostApi.getVolume = &hostGetVolume;
    hostApi.setVolume = &hostSetVolume;
    hostApi.getFps = &hostGetFps;
    hostApi.getVideoWidth = &hostGetVideoWidth;
    hostApi.getVideoHeight = &hostGetVideoHeight;
    hostApi.getMoveStepTime = &hostGetMoveStepTime;
    hostApi.getAxisRoles = &hostGetAxisRoles;
    hostApi.getActiveAxisRole = &hostGetActiveAxisRole;
    hostApi.getAxisActions = &hostGetAxisActions;
    hostApi.getAxisActionCount = &hostGetAxisActionCount;
    hostApi.commitAxisActions = &hostCommitAxisActions;
    hostApi.getAxisSelection = &hostGetAxisSelection;
    hostApi.getAxisSelectionCount = &hostGetAxisSelectionCount;
    hostApi.setAxisSelection = &hostSetAxisSelection;
    hostApi.clearAxisSelection = &hostClearAxisSelection;
    hostApi.seekTo = &hostSeekTo;
    hostApi.setPlaying = &hostSetPlaying;
    hostApi.setSpeed = &hostSetSpeed;
    hostApi.registerCommand = &hostRegisterCommand;
    hostApi.uiLabel = &hostUiLabel;
    hostApi.uiButton = &hostUiButton;
    hostApi.uiCheckbox = &hostUiCheckbox;
    hostApi.uiSliderFloat = &hostUiSliderFloat;
    hostApi.uiSliderInt = &hostUiSliderInt;
    hostApi.uiCombo = &hostUiCombo;
    hostApi.uiSeparator = &hostUiSeparator;
    hostApi.uiPushSection = &hostUiPushSection;
    hostApi.uiPopSection = &hostUiPopSection;
    hostApi.uiRadioButton = &hostUiRadioButton;
    hostApi.uiInputInt = &hostUiInputInt;
    hostApi.uiInputFloat = &hostUiInputFloat;
    hostApi.uiDragInt = &hostUiDragInt;
    hostApi.uiDragFloat = &hostUiDragFloat;
    hostApi.uiInputText = &hostUiInputText;
    hostApi.uiPushRow = &hostUiPushRow;
    hostApi.uiPopRow = &hostUiPopRow;
    hostApi.registerNode = &hostRegisterNode;
    hostApi.nodeInputCount = &nodeInputCount;
    hostApi.nodeInputTime = &nodeInputTime;
    hostApi.nodeInputPosition = &nodeInputPosition;
    hostApi.nodeAddAction = &nodeAddAction;
    hostApi.hostLog = &hostLog;
    hostApi.hostReportFault = &hostReportFault;
    hostApi.hostNotify = &hostNotify;
    hostApi.setActiveAxis = &hostSetActiveAxis;
    hostApi.isAxisVisible = &hostIsAxisVisible;
    hostApi.isAxisLocked = &hostIsAxisLocked;
    hostApi.getAxisName = &hostGetAxisName;
    hostApi.isProjectDirty = &hostIsProjectDirty;
    hostApi.getProjectMetadata = &hostGetProjectMetadata;
    hostApi.getChapterCount = &hostGetChapterCount;
    hostApi.getChapter = &hostGetChapter;
    hostApi.getBookmarkCount = &hostGetBookmarkCount;
    hostApi.getBookmark = &hostGetBookmark;
    hostApi.getRegionCount = &hostGetRegionCount;
    hostApi.getRegion = &hostGetRegion;
    hostApi.getActiveLanguage = &hostGetActiveLanguage;
    hostApi.openFileDialog = &hostOpenFileDialog;
    hostApi.openFilesDialog = &hostOpenFilesDialog;
    hostApi.saveFileDialog = &hostSaveFileDialog;
    hostApi.pickFolder = &hostPickFolder;
    hostApi.confirmDialog = &hostConfirmDialog;
    hostApi.uiColorEdit = &hostUiColorEdit;
    hostApi.uiInputTextMultiline = &hostUiInputTextMultiline;
    hostApi.uiProgressBar = &hostUiProgressBar;
    hostApi.nodeUiSetState = &hostNodeUiSetState;
    hostApi.nodeUiGetState = &hostNodeUiGetState;
    hostApi.nodeUiCurrentKey = &hostNodeUiCurrentKey;
    hostApi.getProjectData = &hostGetProjectData;
    hostApi.setProjectData = &hostSetProjectData;
    hostApi.getAppData = &hostGetAppData;
    hostApi.setAppData = &hostSetAppData;
    hostApi.registerEditMode = &hostRegisterEditMode;
    hostApi.registerNavigator = &hostRegisterNavigator;
    hostApi.isEditModeActive = &hostIsEditModeActive;
    hostApi.isNavigatorActive = &hostIsNavigatorActive;
    hostApi.registerSelectionMode = &hostRegisterSelectionMode;
    hostApi.isSelectionModeActive = &hostIsSelectionModeActive;
    hostApi.uiTextWrapped = &hostUiTextWrapped;
    hostApi.uiPushDisabled = &hostUiPushDisabled;
    hostApi.uiPopDisabled = &hostUiPopDisabled;
    hostApi.uiTooltip = &hostUiTooltip;
    hostApi.uiPushDisabledTooltip = &hostUiPushDisabledTooltip;
    hostApi.uiPopDisabledTooltip = &hostUiPopDisabledTooltip;
    hostApi.getFunscriptJson = &hostGetFunscriptJson;

    // Node-body custom-UI invoker. Runs synchronously during the
    // ProcessingPanel node render, so ImGui state is live and the host UI widgets draw into the
    // node body. Form-section mode (uiSectionDepth = 1) lays the node's `ui`-callback widgets into
    // the 2-column param table ProcessingPanel opens around this call (it owns BeginTable/EndTable).
    effectReg_.renderNodeUi = [this](const PluginNodeEntry &entry, std::string &nodeState, int regionId,
                                     int nodeId) -> bool {
        if (!entry.onNodeUi)
            return false;
        callCtx_.currentNodeState = &nodeState;
        callCtx_.currentNodeStateChanged = false;
        callCtx_.currentNodeRegionId = regionId; // surfaced to managed via nodeUiCurrentKey for Node.Update
        callCtx_.currentNodeId = nodeId;
        callCtx_.uiIdCounter = 0;
        callCtx_.uiSectionDepth = 1;
        callCtx_.inRow = false;
        entry.onNodeUi(&callCtx_, entry.userData);
        callCtx_.uiSectionDepth = 0;
        callCtx_.currentNodeState = nullptr;
        callCtx_.currentNodeRegionId = -1;
        callCtx_.currentNodeId = -1;
        bool changed = callCtx_.currentNodeStateChanged;
        callCtx_.currentNodeStateChanged = false;
        return changed;
    };

    eq.on<SetPluginEnabledEvent>([this](const SetPluginEnabledEvent &e) { onSetPluginEnabled(e); });
    eq.on<SetPluginHotReloadEvent>([this](const SetPluginHotReloadEvent &e) { onSetPluginHotReload(e); });
    eq.on<RequestInstallPluginEvent>([this](const RequestInstallPluginEvent &e) { onRequestInstallPlugin(e); });
    // ^ onRequestInstallPlugin is a co::Fire (it may co_await the zip picker); the handler just kicks it off.
    eq.on<RequestUninstallPluginEvent>([this](const RequestUninstallPluginEvent &e) { onRequestUninstallPlugin(e); });
    eq.on<SavePluginStatesEvent>([this](const SavePluginStatesEvent &e) { onSavePluginStates(e); });
    eq.on<RegisterPluginNodeEvent>([this](const RegisterPluginNodeEvent &e) { onRegisterPluginNode(e); });
    eq.on<UnregisterPluginNodesEvent>([this](const UnregisterPluginNodesEvent &e) { onUnregisterPluginNodes(e); });
    eq.on<PlayStateChangedEvent>([this](const PlayStateChangedEvent &e) { onPlayStateChanged(e); });
    eq.on<SpeedChangedEvent>([this](const SpeedChangedEvent &e) { onSpeedChanged(e); });
    eq.on<MediaChangedEvent>([this](const MediaChangedEvent &e) { onMediaChanged(e); });
    eq.on<SetLanguageEvent>([this](const SetLanguageEvent &e) { onSetLanguage(e); });
    eq.on<ReloadPluginsForLanguageEvent>(
        [this](const ReloadPluginsForLanguageEvent &e) { onReloadPluginsForLanguage(e); });
    eq.on<LoadProjectEvent>([this](const LoadProjectEvent &e) { onProjectLoaded(e); });
    eq.on<AxisModifiedEvent>([this](const AxisModifiedEvent &e) { onAxisModified(e); });
    eq.on<AxisSelectedEvent>([this](const AxisSelectedEvent &e) { onAxisSelected(e); });
    eq.on<ChangeDummyDurationEvent>([this](const ChangeDummyDurationEvent &) { callCtx_.player = dummyPlayer_.get(); });
    eq.on<LoadVideoEvent>([this](const LoadVideoEvent &) { callCtx_.player = player_.get(); });
    eq.on<CloseVideoEvent>([this](const CloseVideoEvent &) { callCtx_.player = player_.get(); });
}

PluginManager::~PluginManager() {
    shutdown();
}

void PluginManager::shutdown() {
    if (shutdownDone_)
        return;
    shutdownDone_ = true;

    // Final flush of any app-scoped settings still dirty (e.g. changed since the last update() flush)
    // before the plugins are torn down — the on-close half of the eager/per-frame persistence.
    flushDirtyAppSettings();

    // Tear down every ENABLED plugin while hostApi/callCtx_ are still alive (the managed host holds raw
    // pointers into them). Deliberately does NOT flip lp.enabled, does NOT savePluginStates(), and pushes
    // NO events: shutdown is not a user "disable", so each plugin must stay persisted as enabled and reload
    // next launch, and there is no frame loop left to drain a queue. Mirrors setPluginEnabled(false)'s
    // native teardown minus those side effects.
    for (auto &lp : loadedPlugins) {
        if (!lp.enabled)
            continue;
        if (lp.api.onUnload)
            lp.api.onUnload();
        if (unloadPluginNative)
            unloadPluginNative(lp.name.c_str(), /*finalShutdown=*/1); // app closing: skip force-collect + warn
        std::memset(&lp.api, 0, sizeof(PluginApi));
    }
}

void PluginManager::notifyPluginFault(const std::string &who, const std::string &ctx) {
    const auto suppressed = faultThrottle_.onFault(who);
    if (!suppressed) // inside the coalescing window — counted, not emitted
        return;
    std::string message = *suppressed > 0
                              ? fmt::format("Plugin '{}' error in {} (+{} more) — see log", who, ctx, *suppressed)
                              : fmt::format("Plugin '{}' error in {} — see log", who, ctx);
    eventQueue_.push(NotifyEvent{.level = NotifyLevel::Error, .message = std::move(message)});
}

void PluginManager::notifyPlugin(const std::string &who, NotifyLevel level, const std::string &msg) {
    // Plugin-initiated, deliberate toast: show the message verbatim (prefixed with the plugin name so
    // the user knows the source) at the plugin's chosen level. Unlike notifyPluginFault this is not
    // throttled — a plugin emitting one per frame is its own bug, not a swallowed-fault storm.
    eventQueue_.push(NotifyEvent{.level = level, .message = fmt::format("[{}] {}", who, msg)});
}

bool PluginManager::init() {
    // The managed host runs every plugin with full privileges. Refuse to bring it up if the host or
    // the API assembly was tampered with (hash mismatch vs this build) — a hard stop, not a prompt.
    // Ofs.Api isn't loaded by us directly (the CLR resolves it), so it is checked on disk.
    if (!managedAssemblyTrusted("managed/Ofs.Api.dll", "Ofs.Api") ||
        !managedAssemblyTrusted("managed/Ofs.PluginHost.dll", "Ofs.PluginHost")) {
        OFS_CORE_ERROR("Managed host integrity check failed; plugin system disabled.");
        return false;
    }

    if (!dotNetHost.init()) {
        OFS_CORE_ERROR("Failed to initialize .NET Runtime.");
        return false;
    }

    std::filesystem::path hostPath = "managed/Ofs.PluginHost.dll";
    if (!dotNetHost.loadAssembly(hostPath)) {
        OFS_CORE_ERROR("Failed to load Ofs.PluginHost.dll");
        return false;
    }

    loadPluginNative = dotNetHost.getFunctionPointer<load_plugin_native_fn>(
        hostPath, STR("Ofs.PluginHost.PluginBootstrapper, Ofs.PluginHost"), STR("LoadPluginNative"));

    if (!loadPluginNative) {
        OFS_CORE_ERROR("Failed to get LoadPluginNative function pointer.");
        return false;
    }

    unloadPluginNative = dotNetHost.getFunctionPointer<unload_plugin_native_fn>(
        hostPath, STR("Ofs.PluginHost.PluginBootstrapper, Ofs.PluginHost"), STR("UnloadPluginNative"));

    if (!unloadPluginNative) {
        OFS_CORE_WARN("Failed to get UnloadPluginNative function pointer — disable/enable will not work.");
    }

    // Plugin-node TState capture/release codec. ProcessingSystem reads
    // these off effectReg at snapshot build / eval completion; leaving them null disables TState nodes
    // (they resolve but capture nothing). Plain [UnmanagedCallersOnly] entry points, like the loaders.
    auto captureFn = dotNetHost.getFunctionPointer<OfsCaptureNodeStateFn>(
        hostPath, STR("Ofs.PluginHost.PluginBootstrapper, Ofs.PluginHost"), STR("CaptureNodeStateNative"));
    auto releaseFn = dotNetHost.getFunctionPointer<OfsReleaseNodeStatesFn>(
        hostPath, STR("Ofs.PluginHost.PluginBootstrapper, Ofs.PluginHost"), STR("ReleaseNodeStatesNative"));
    auto clearUpdatesFn = dotNetHost.getFunctionPointer<OfsClearNodeUpdatesFn>(
        hostPath, STR("Ofs.PluginHost.PluginBootstrapper, Ofs.PluginHost"), STR("ClearNodeUpdatesNative"));
    if (captureFn && releaseFn) {
        effectReg_.nodeStateCodec.capture = captureFn;
        effectReg_.nodeStateCodec.release = releaseFn;
        effectReg_.nodeStateCodec.clearNodeUpdates = clearUpdatesFn; // may be null on an older host; call-guarded
    } else {
        OFS_CORE_WARN("Failed to fetch plugin-node state codec; plugin TState nodes will not carry state.");
    }

    // Managed heap readout for the footer. Optional: a null pointer (older host) just hides the zone.
    getManagedHeapBytesNative = dotNetHost.getFunctionPointer<get_managed_heap_bytes_fn>(
        hostPath, STR("Ofs.PluginHost.PluginBootstrapper, Ofs.PluginHost"), STR("GetManagedHeapBytesNative"));

    OFS_CORE_INFO(".NET Runtime and PluginHost initialized.");
    return true;
}

long long PluginManager::managedHeapBytes() const {
    if (!getManagedHeapBytesNative)
        return 0;
    const long long bytes = getManagedHeapBytesNative();
    return bytes > 0 ? bytes : 0;
}

static std::filesystem::path getPluginStatesPath() {
    return ofs::util::getPrefPath() / "plugin_states.json";
}

// Plugins whose folder couldn't be deleted at uninstall time (DLL momentarily still locked). They are
// removed on the next launch — before anything loads, when the files are free — so an uninstall always
// completes on its own and a stuck one never resurrects. Persisted next to plugin_states.json.
static std::filesystem::path getPendingUninstallPath() {
    return ofs::util::getPrefPath() / "plugins_pending_uninstall.json";
}

static std::set<std::string> loadPendingUninstalls() {
    std::set<std::string> result;
    auto text = ofs::util::readFile(getPendingUninstallPath());
    if (!text)
        return result;
    try {
        for (const auto &entry : nlohmann::json::parse(*text))
            result.insert(entry.get<std::string>());
    } catch (const std::exception &e) {
        OFS_CORE_ERROR("Failed to load pending plugin uninstalls: {}", e.what());
    }
    return result;
}

static void savePendingUninstalls(const std::set<std::string> &names) {
    std::error_code ec;
    if (names.empty()) {
        std::filesystem::remove(getPendingUninstallPath(), ec);
        return;
    }
    nlohmann::json j = nlohmann::json::array();
    for (const auto &n : names)
        j.push_back(n);
    ofs::util::writeFileAtomic(getPendingUninstallPath(), j.dump(2));
}

std::map<std::string, PluginManager::PluginSavedState> PluginManager::loadPluginStates() const {
    std::map<std::string, PluginSavedState> result;
    try {
        auto text = ofs::util::readFile(getPluginStatesPath());
        if (!text)
            return result;
        auto j = nlohmann::json::parse(*text);
        for (const auto &entry : j) {
            std::string name = entry.at("name").get<std::string>();
            PluginSavedState state;
            state.enabled = entry.value("enabled", true);
            state.windowOpen = entry.value("windowOpen", true);
            state.acknowledgedHash = entry.value("acknowledgedHash", std::string{});
            state.version = entry.value("version", std::string{});
            result[name] = state;
        }
    } catch (const std::exception &e) {
        OFS_CORE_ERROR("Failed to load plugin states: {}", e.what());
    }
    return result;
}

void PluginManager::savePluginStates() const {
    try {
        nlohmann::json j = nlohmann::json::array();
        for (const auto &plugin : loadedPlugins) {
            j.push_back({
                {"name", plugin.name},
                {"enabled", plugin.enabled},
                {"windowOpen", plugin.windowOpen},
                {"acknowledgedHash", plugin.acknowledgedHash},
                {"version", plugin.version},
            });
        }
        ofs::util::writeFileAtomic(getPluginStatesPath(), j.dump(4));
    } catch (const std::exception &e) {
        OFS_CORE_ERROR("Failed to save plugin states: {}", e.what());
    }
}

std::optional<std::string> PluginManager::hashPluginDll(const LoadedPlugin &lp) const {
    auto bytes = ofs::util::readFile(lp.path);
    if (!bytes) {
        OFS_CORE_ERROR("Cannot read plugin DLL for trust check: {}", ofs::util::toUtf8(lp.path));
        return std::nullopt;
    }
    return picosha2::hash256_hex_string(*bytes);
}

bool PluginManager::isPluginTrusted(const LoadedPlugin &lp) const {
#ifdef OFS_PLUGIN_TEST_HOOKS
    // Tests run headless and load through this path with no UI; auto-trust. addTestPlugin is the
    // usual seam, but any test that does call the real loader still works.
    return true;
#else
    auto hash = hashPluginDll(lp);
    if (!hash)
        return false;
    // Shipped first-party plugins carry a SHA-256 baked into the binary at build time; one whose
    // name and bytes both match is trusted without consent (the user already trusts the app itself).
    if (firstPartyHashMatches(lp.name, *hash))
        return true;
    return lp.acknowledgedHash == *hash; // unchanged bytes the user previously consented to
#endif
}

std::string PluginManager::trustPromptMessage(const std::string &name, const std::filesystem::path &dir) {
    return std::format("Plugin {} wants to load from:\n{}\n\n"
                       "Plugins run with full access to your computer — files, network, everything you "
                       "can do. We cannot verify a plugin is safe. Only load plugins from sources you trust.",
                       name, ofs::util::toUtf8(dir));
}

void PluginManager::loadFromDir(const std::filesystem::path &pluginDir,
                                const std::map<std::string, PluginSavedState> &savedStates, bool firstParty) {
    // dir "foo/" holds "foo.dll" — build "foo.dll" by appending to the (lossless) native name.
    std::filesystem::path pluginDll = pluginDir / pluginDir.filename();
    pluginDll += ofs::util::fromUtf8(".dll");
    if (!std::filesystem::exists(pluginDll))
        return;

    LoadedPlugin lp;
    lp.name = ofs::util::toUtf8(pluginDll.stem());
    lp.displayName = lp.name;
    lp.path = pluginDll;
    lp.firstParty = firstParty;

    auto it = savedStates.find(lp.name);
    bool savedEnabled = (it == savedStates.end()) || it->second.enabled;
    bool savedWindowOpen = (it == savedStates.end()) || it->second.windowOpen;
    if (it != savedStates.end()) {
        lp.acknowledgedHash = it->second.acknowledgedHash;
        lp.version = it->second.version; // shown for disabled stubs; doLoad refreshes it on a real load
    }

    // Startup never prompts: a plugin auto-loads only if it is already trusted — first-party
    // (baked hash) or its current bytes match a hash the user previously consented to. Anything
    // unacknowledged or byte-changed stays a disabled stub until the user explicitly enables it
    // (which is where the trust modal lives). This is why trust == the explicit enable action.
    if (savedEnabled && isPluginTrusted(lp)) {
        if (doLoad(lp)) {
            lp.windowOpen = savedWindowOpen;
            OFS_CORE_INFO("Loaded plugin: {0}", lp.name);
            loadedPlugins.push_back(std::move(lp));
        }
    } else {
        lp.enabled = false;
        lp.windowOpen = false;
        OFS_CORE_INFO("Plugin {0} not loaded (disabled or trust declined)", lp.name);
        loadedPlugins.push_back(std::move(lp));
    }
}

void PluginManager::loadPlugins() {
    auto savedStates = loadPluginStates();

    const std::filesystem::path userRoot = ofs::util::getPrefPath() / "plugins";

    // Finish any uninstall whose files couldn't be deleted last session (the DLL was still locked).
    // Nothing is loaded yet, so the folders are free now. Anything that still won't delete stays in
    // the set and is skipped below, so a stuck uninstall never resurrects.
    auto pendingUninstall = loadPendingUninstalls();
    if (!pendingUninstall.empty()) {
        std::set<std::string> stillPending;
        for (const auto &name : pendingUninstall) {
            const std::filesystem::path dir = userRoot / ofs::util::fromUtf8(name);
            if (!std::filesystem::exists(dir) || removeDirRetry(dir))
                OFS_CORE_INFO("Completed deferred uninstall of plugin: {}", name);
            else
                stillPending.insert(name);
        }
        savePendingUninstalls(stillPending);
        pendingUninstall = std::move(stillPending);
    }
    pendingUninstall_ = pendingUninstall; // remembered so discoverNewPlugins won't re-surface these

    // Base root: shipped first-party plugins ONLY, loaded by name from the baked-in allowlist — the
    // directory is never scanned. It lives under the managed dir (getBasePath()/managed/plugins), not a
    // top-level plugins/ next to the executable, so the only plugins/ folder a user sees is their own
    // writable <pref>/plugins (a bundled folder next to the app reads as "drop plugins here", but this
    // root never scans, so a dropped plugin would silently never load). A DLL someone drops here that is
    // not on the allowlist is ignored. Name + baked-hash is the gate (see isPluginTrusted /
    // firstPartyHashMatches).
    const std::filesystem::path baseRoot = ofs::util::getBasePath() / "managed" / "plugins";
    for (const std::string_view name : firstPartyPluginNames()) {
        const std::filesystem::path dir = baseRoot / ofs::util::fromUtf8(name);
        if (std::filesystem::exists(dir))
            loadFromDir(dir, savedStates, /*firstParty=*/true);
    }

    // User root (<pref>/plugins): the only scanned, writable root — everything the user installs.
    // Each plugin goes through the trust prompt. A directory whose name is reserved for a first-party
    // plugin is ignored so a user plugin can never shadow or impersonate a shipped one.
    if (std::filesystem::exists(userRoot)) {
        for (const auto &entry : std::filesystem::directory_iterator(userRoot)) {
            if (!entry.is_directory())
                continue;
            const std::string name = ofs::util::toUtf8(entry.path().filename());
            if (isFirstPartyName(name)) {
                OFS_CORE_WARN("Ignoring user plugin '{}' — name is reserved for a first-party plugin.", name);
                continue;
            }
            if (pendingUninstall.contains(name))
                continue; // uninstall pending but its files couldn't be removed yet — do not load
            loadFromDir(entry.path(), savedStates, /*firstParty=*/false);
        }
    }

    // Persist any newly-recorded acknowledgments and decline-driven disables.
    savePluginStates();
}

bool PluginManager::doLoad(LoadedPlugin &lp, bool notifyOnFailure) {
    auto pathStr = ofs::util::toUtf8(std::filesystem::absolute(lp.path)); // C# reads it as UTF-8
    std::memset(&lp.api, 0, sizeof(PluginApi));

    callCtx_.currentPluginName = lp.name.c_str();
    callCtx_.inOnLoad = true;
    int rc = loadPluginNative(pathStr.c_str(), &lp.api, &hostApi);
    callCtx_.inOnLoad = false;
    callCtx_.currentPluginName = nullptr;

    // A trusted plugin we tried to load and couldn't is worth a word to the user — otherwise the failure
    // lives only in the log. The incompatible-build case is the one the user can act on (rebuild, or
    // update ofs-ng), so it gets a distinct message; every other code is an internal load fault with
    // nothing actionable to add. rc -8 is the Ofs.Api assembly-version gate in PluginBootstrapper.
    auto notifyFailure = [&](const std::string &message) {
        if (notifyOnFailure)
            eventQueue_.push(NotifyEvent{.level = NotifyLevel::Error, .message = message});
    };

    if (rc != 0) {
        OFS_CORE_ERROR("Failed to load plugin {0}, error code: {1}", lp.name, rc);
        notifyFailure(rc == -8 ? fmt::format("Plugin {} isn't compatible with this version of ofs-ng. "
                                             "Rebuild it against this ofs-ng build.",
                                             lp.displayName)
                               : fmt::format("Plugin {} failed to load.", lp.displayName));
        return false;
    }
    // Native↔managed ABI check: the PluginApi struct the bridge filled must match this
    // build's layout. (Plugin-vs-host API compatibility is enforced separately, by the
    // Ofs.Api assembly-version check in PluginBootstrapper.)
    if (!isPluginAbiVersionSupported(lp.api.version)) {
        OFS_CORE_ERROR("Plugin {0} unsupported ABI version: {1}", lp.name, lp.api.version);
        std::memset(&lp.api, 0, sizeof(PluginApi));
        notifyFailure(fmt::format("Plugin {} isn't compatible with this version of ofs-ng. "
                                  "Rebuild it against this ofs-ng build.",
                                  lp.displayName));
        return false;
    }
    lp.displayName = (lp.api.getName && lp.api.getName()) ? lp.api.getName() : lp.name;
    lp.version = (lp.api.getVersion && lp.api.getVersion()) ? lp.api.getVersion() : "";
    return true;
}

co::Fire PluginManager::installFromZip(std::filesystem::path zipPath) {
    auto fail = [this](const char *msg) { showError(eventQueue_, "Install plugin", msg); };

    if (!std::filesystem::exists(zipPath)) {
        fail("The selected file no longer exists.");
        co_return;
    }

    // 1. STAGE: extract into a throwaway dir. Never touch <pref>/plugins until everything validates.
    // The guard wipes the staging tree on every exit path (reject, decline, or success).
    std::error_code ec;
    const std::filesystem::path tempRoot = std::filesystem::temp_directory_path(ec);
    if (ec) {
        fail("Could not access a temporary directory for staging.");
        co_return;
    }
    const std::filesystem::path stageRoot = tempRoot / "ofs-plugin-staging";
    std::filesystem::remove_all(stageRoot, ec);
    std::filesystem::create_directories(stageRoot, ec);
    struct StageGuard {
        std::filesystem::path dir;
        ~StageGuard() {
            std::error_code e;
            std::filesystem::remove_all(dir, e);
        }
    } guard{stageRoot};

    if (!extractZip(zipPath, stageRoot)) {
        fail("Could not extract the zip archive.");
        co_return;
    }

    const std::filesystem::path stagedDir = locatePluginDir(stageRoot);
    if (stagedDir.empty()) {
        fail("The zip does not contain a valid plugin folder (expected name/name.dll).");
        co_return;
    }
    const std::filesystem::path stem = stagedDir.filename(); // lossless; name (UTF-8) is for display
    const std::string name = ofs::util::toUtf8(stem);

    // 2. STRUCTURE: the .NET entry trio must be present, else it is not a loadable plugin.
    auto sibling = [&](const char *suffix) {
        std::filesystem::path p = stagedDir / stem;
        p += suffix;
        return p;
    };
    if (!std::filesystem::exists(sibling(".dll")) || !std::filesystem::exists(sibling(".runtimeconfig.json")) ||
        !std::filesystem::exists(sibling(".deps.json"))) {
        fail("Not a valid plugin folder (missing the .dll, .runtimeconfig.json or .deps.json).");
        co_return;
    }

    // 3. COLLIDE: first-party names are reserved; an existing user plugin is the update path.
    if (isFirstPartyName(name)) {
        fail("That name is reserved for a built-in plugin.");
        co_return;
    }
    const std::filesystem::path destDir = ofs::util::getPrefPath() / "plugins" / stem;
    const bool replacing = std::filesystem::exists(destDir);
    if (replacing) {
        const std::string msg = std::format("A plugin named {} is already installed.\n\nReplace it?", name);
        if (co_await Confirm{eventQueue_,
                             {.title = "Install plugin",
                              .message = msg,
                              .buttons = {"Replace", "Cancel"},
                              .severity = ModalSeverity::Warning}} != 0)
            co_return;
    }

    // 4. TRUST: install IS the consent point. Prompt against the staged DLL before anything is
    // committed, so a declined install leaves nothing behind. (Reserved first-party names are
    // rejected at step 3, so an install always needs explicit consent.)
    LoadedPlugin staged;
    staged.name = name;
    staged.path = sibling(".dll");
    auto stagedHash = hashPluginDll(staged);
    if (!stagedHash) {
        fail("Could not read the plugin DLL.");
        co_return;
    }
    if (co_await Confirm{eventQueue_,
                         {.title = "Load plugin?",
                          .message = trustPromptMessage(name, staged.path.parent_path()),
                          .buttons = {"Load", "Cancel"},
                          .severity = ModalSeverity::Warning}} != 0)
        co_return; // declined → nothing committed
    staged.acknowledgedHash = *stagedHash;

    // 5. COMMIT: if replacing a loaded plugin, unload it first so the CLR releases the DLL and the
    // directory can be overwritten; drop its entry so the reload below is clean.
    if (replacing) {
        if (std::ranges::any_of(loadedPlugins, [&](const LoadedPlugin &p) { return p.name == name; }))
            setPluginEnabled(name, false);
        std::erase_if(loadedPlugins, [&](const LoadedPlugin &p) { return p.name == name; });
        std::filesystem::remove_all(destDir, ec);
    }
    std::filesystem::create_directories(destDir.parent_path(), ec);
    if (!moveDir(stagedDir, destDir)) {
        fail("Could not write into the plugins folder.");
        co_return;
    }

    // 6. LOAD: hot-load from the committed location. The acknowledged hash carries over from the
    // staged DLL (same bytes), so isPluginTrusted passes without a second prompt.
    LoadedPlugin lp;
    lp.name = name;
    lp.displayName = name;
    lp.path = destDir / stem;
    lp.path += ofs::util::fromUtf8(".dll");
    lp.firstParty = false;
    lp.acknowledgedHash = staged.acknowledgedHash;
    // notifyOnFailure=false: the else branch below surfaces a load failure through its own install modal.
    if (isPluginTrusted(lp) && doLoad(lp, /*notifyOnFailure=*/false)) { // staged bytes → trusted, no re-prompt
        OFS_CORE_INFO("Installed and loaded plugin: {0}", name);
        loadedPlugins.push_back(std::move(lp));
        savePluginStates();
        eventQueue_.push(
            NotifyEvent{.level = NotifyLevel::Success, .message = std::format("Installed plugin {}.", name)});
    } else {
        savePluginStates();
        fail("The plugin was installed but failed to load.");
    }
}

co::Fire PluginManager::onRequestInstallPlugin(RequestInstallPluginEvent e) {
    std::filesystem::path zip;
    if (!e.path.empty()) {
        zip = ofs::util::fromUtf8(e.path);
    } else {
        std::string sel = co_await FileDialog{eventQueue_,
                                              {.kind = FileDialogKind::Open,
                                               .key = "plugin_zip",
                                               .title = "Install plugin from zip",
                                               .filterPatterns = {"*.zip"},
                                               .filterDesc = "Plugin zip (*.zip)"}};
        if (sel.empty())
            co_return;
        zip = ofs::util::fromUtf8(sel); // dialog results are UTF-8
    }
    installFromZip(std::move(zip));
}

co::Fire PluginManager::onRequestUninstallPlugin(RequestUninstallPluginEvent e) {
    auto it = std::ranges::find_if(loadedPlugins, [&](const LoadedPlugin &p) { return p.name == e.name; });
    if (it == loadedPlugins.end())
        co_return;
    if (it->firstParty || isFirstPartyName(e.name)) {
        showWarning(eventQueue_, "Uninstall plugin", "Built-in plugins cannot be uninstalled.");
        co_return;
    }

    // The plugin directory is the entry DLL's parent — capture it (by value) before the await; the
    // `it` iterator must not be used after the co_await (loadedPlugins may change meanwhile).
    const std::filesystem::path dir = it->path.parent_path();
    const std::string msg =
        std::format("Uninstall plugin {}?\n\nThis deletes it from\n{}", e.name, ofs::util::toUtf8(dir));
    if (co_await Confirm{eventQueue_,
                         {.title = "Uninstall plugin",
                          .message = msg,
                          .buttons = {"Uninstall", "Cancel"},
                          .severity = ModalSeverity::Warning}} != 0)
        co_return;

    // Unload first so the CLR releases the DLL, then drop the entry and delete the folder. Removing
    // the entry also drops it from plugin_states.json on the save below.
    // Unloading releases the CLR's lock on the DLL (UnloadPluginNative forces the collectible load
    // context to be collected), so the directory is normally deletable right after.
    setPluginEnabled(e.name, false);
    std::erase_if(loadedPlugins, [&](const LoadedPlugin &p) { return p.name == e.name; });
    savePluginStates();

    if (removeDirRetry(dir)) {
        pendingUninstall_.erase(e.name);
        OFS_CORE_INFO("Uninstalled plugin: {0}", e.name);
        eventQueue_.push(
            NotifyEvent{.level = NotifyLevel::Info, .message = std::format("Uninstalled plugin {}.", e.name)});
        co_return;
    }

    // The DLL is still locked (rare — e.g. a plugin left a background thread running). The plugin is
    // already gone from the UI and won't load again; record it so its files are deleted automatically
    // on the next launch, before anything loads. The user is never asked to delete anything by hand.
    OFS_CORE_WARN("Plugin {} folder still locked; deferring deletion to next launch: {}", e.name,
                  ofs::util::toUtf8(dir));
    auto pending = loadPendingUninstalls();
    pending.insert(e.name);
    savePendingUninstalls(pending);
    pendingUninstall_.insert(e.name); // keep discoverNewPlugins from re-surfacing the still-present folder
    eventQueue_.push(
        NotifyEvent{.level = NotifyLevel::Warning,
                    .message = std::format("Uninstalled {}. Remaining files are removed on the next launch.", e.name)});
}

void PluginManager::setPluginEnabled(const std::string &name, bool enable) {
    auto it = std::ranges::find_if(loadedPlugins, [&](const LoadedPlugin &p) { return p.name == name; });
    if (it == loadedPlugins.end() || it->enabled == enable)
        return;

    LoadedPlugin &lp = *it;

    if (!enable) {
        if (lp.api.onUnload)
            lp.api.onUnload();
        if (unloadPluginNative)
            unloadPluginNative(lp.name.c_str(), /*finalShutdown=*/0); // runtime disable: force-collect to free the DLL
        std::memset(&lp.api, 0, sizeof(PluginApi));
        lp.windowOpen = false;
        lp.enabled = false;
        // Drop this plugin's commands synchronously, mirroring the direct add() in hostRegisterCommand.
        // A same-frame reload (install-replace, hot-reload) re-registers during doLoad before any event
        // drains: a deferred removal would both collide with the stale entry and then clobber the
        // freshly re-registered one. The event below now only tears down this plugin's key bindings.
        if (callCtx_.commandRegistry)
            callCtx_.commandRegistry->removeByGroup(lp.name);
        eventQueue_.push(UnregisterPluginShortcutsEvent{lp.name});
        eventQueue_.push(UnregisterPluginNodesEvent{lp.name});
        eventQueue_.push(UnregisterEditModesEvent{lp.name});
        eventQueue_.push(UnregisterNavigatorsEvent{lp.name});
        eventQueue_.push(UnregisterSelectionModesEvent{lp.name});
        OFS_CORE_INFO("Disabled plugin: {0}", lp.name);
    } else if (isPluginTrusted(lp)) {
        // Enable only loads an already-trusted plugin. An untrusted one must go through
        // enablePluginWithConsent first (the modal), which records the hash and calls back here.
        eventQueue_.push(LoadShortcutBindingsEvent{});
        if (doLoad(lp)) {
            lp.enabled = true;
            lp.windowOpen = true;
            OFS_CORE_INFO("Enabled plugin: {0}", lp.name);
        }
    }
    savePluginStates();
}

co::Fire PluginManager::enablePluginWithConsent(std::string name) {
    auto find = [&] {
        return std::ranges::find_if(loadedPlugins, [&](const LoadedPlugin &p) { return p.name == name; });
    };
    auto it = find();
    if (it == loadedPlugins.end() || it->enabled)
        co_return;

    // Untrusted (new or byte-changed) plugins need explicit, current consent before any code runs.
    if (!isPluginTrusted(*it)) {
        auto hash = hashPluginDll(*it);
        if (!hash)
            co_return;
        const std::string msg = trustPromptMessage(it->name, it->path.parent_path());
        if (co_await Confirm{eventQueue_,
                             {.title = "Load plugin?",
                              .message = msg,
                              .buttons = {"Load", "Cancel"},
                              .severity = ModalSeverity::Warning}} != 0) {
            OFS_CORE_INFO("User declined to load plugin: {0}", name);
            co_return;
        }
        // The await may have mutated loadedPlugins (install/uninstall); re-find before writing.
        it = find();
        if (it == loadedPlugins.end())
            co_return;
        it->acknowledgedHash = *hash; // record consent so future loads (incl. startup) are silent
    }
    setPluginEnabled(name, true); // now trusted → loads without prompting
}

void PluginManager::onSetPluginHotReload(const SetPluginHotReloadEvent &e) {
    auto it = std::ranges::find_if(loadedPlugins, [&](const LoadedPlugin &p) { return p.name == e.name; });
    if (it == loadedPlugins.end() || it->hotReload == e.enabled)
        return;
    it->hotReload = e.enabled;
    it->hotReloadMtime = {}; // re-seed lazily on the next poll either way → no reload on the toggle itself
    OFS_CORE_INFO("Plugin hot-reload {} for {}", e.enabled ? "enabled" : "disabled", e.name);
}

void PluginManager::discoverNewPlugins() {
    std::error_code ec;
    const std::filesystem::path userRoot = ofs::util::getPrefPath() / "plugins";
    if (!std::filesystem::exists(userRoot, ec))
        return;

    // Saved states are only needed when a genuinely new folder turns up (rare), so read them lazily —
    // this keeps the steady-state poll to a single directory scan.
    std::map<std::string, PluginSavedState> savedStates;
    bool savedLoaded = false;
    for (const auto &entry : std::filesystem::directory_iterator(userRoot, ec)) {
        if (ec)
            break;
        if (!entry.is_directory())
            continue;
        const std::string name = ofs::util::toUtf8(entry.path().filename());
        if (isFirstPartyName(name) || pendingUninstall_.contains(name))
            continue;
        if (std::ranges::any_of(loadedPlugins, [&](const LoadedPlugin &p) { return p.name == name; }))
            continue; // already known this session
        if (!savedLoaded) {
            savedStates = loadPluginStates();
            savedLoaded = true;
        }
        // loadFromDir is a no-op until <name>/<name>.dll exists, so a folder mid-copy is simply retried
        // on the next poll. Trusted bytes load now; anything else becomes a disabled stub to enable.
        loadFromDir(entry.path(), savedStates, /*firstParty=*/false);
        if (std::ranges::any_of(loadedPlugins, [&](const LoadedPlugin &p) { return p.name == name; }))
            OFS_CORE_INFO("Discovered new user plugin at runtime: {}", name);
    }
}

void PluginManager::pollHotReload() {
    for (auto &lp : loadedPlugins) {
        // Only live, user-installed plugins with the toggle on: a disabled one has no instance to swap,
        // and shipped first-party plugins are immutable (gated by baked hash, which a rebuild can't match).
        if (!lp.hotReload || !lp.enabled || lp.firstParty)
            continue;
        std::error_code ec;
        const auto mtime = std::filesystem::last_write_time(lp.path, ec);
        if (ec)
            continue; // DLL momentarily absent (mid-rebuild) — try again next poll
        if (lp.hotReloadMtime == std::filesystem::file_time_type{}) {
            lp.hotReloadMtime = mtime; // first sight after the toggle: record the baseline, don't reload
            continue;
        }
        if (mtime == lp.hotReloadMtime)
            continue;
        // Bytes changed. Hashing reads the whole file, so a successful read also confirms the writer is
        // done (a half-written DLL fails the read) — only then commit the new mtime and reload.
        auto hash = hashPluginDll(lp);
        if (!hash)
            continue; // still being written; retry on the next poll
        lp.hotReloadMtime = mtime;
        reloadPlugin(lp, *hash);
    }
}

// Tear a live plugin instance down exactly as a runtime disable does (force-collect so the CLR releases
// the DLL before any reload from the same path) and drop its host-side registrations. Shared by the
// hot-reload path and the language-switch reload. Leaves lp.api zeroed; the caller decides whether and
// how to doLoad() again.
void PluginManager::tearDownPlugin(LoadedPlugin &lp) {
    if (lp.api.onUnload)
        lp.api.onUnload();
    if (unloadPluginNative)
        unloadPluginNative(lp.name.c_str(), /*finalShutdown=*/0);
    std::memset(&lp.api, 0, sizeof(PluginApi));
    // Synchronous command removal (symmetric with the direct add() in hostRegisterCommand): reloadPlugin
    // and the install-replace path doLoad() in the same frame, so a deferred removal would clobber the
    // re-registered commands. See setPluginEnabled for the full rationale.
    if (callCtx_.commandRegistry)
        callCtx_.commandRegistry->removeByGroup(lp.name);
    eventQueue_.push(UnregisterPluginShortcutsEvent{lp.name});
    eventQueue_.push(UnregisterPluginNodesEvent{lp.name});
    eventQueue_.push(UnregisterEditModesEvent{lp.name});
    eventQueue_.push(UnregisterNavigatorsEvent{lp.name});
    eventQueue_.push(UnregisterSelectionModesEvent{lp.name});
}

void PluginManager::reloadPlugin(LoadedPlugin &lp, const std::string &newHash) {
    OFS_CORE_INFO("Hot-reloading plugin: {0}", lp.name);

    tearDownPlugin(lp);

    // Leaving hot-reload on is the developer's standing consent for their own rebuilds, so accept the new
    // bytes without re-prompting. Persisted so a later normal startup of these exact bytes stays silent.
    lp.acknowledgedHash = newHash;
    eventQueue_.push(LoadShortcutBindingsEvent{});
    if (doLoad(lp)) {
        lp.enabled = true;
        savePluginStates();
        eventQueue_.push(
            NotifyEvent{.level = NotifyLevel::Success, .message = std::format("Reloaded plugin {}.", lp.displayName)});
    } else {
        lp.enabled = false; // doLoad surfaced the reason (incompatible build vs. generic fault) as a toast
    }
}

// <pref>/plugin_settings/<plugin>.json — one app-settings file per plugin.
static std::filesystem::path appSettingsPath(const std::string &pluginName) {
    return ofs::util::getPrefPath() / "plugin_settings" / ofs::util::fromUtf8(pluginName + ".json");
}

nlohmann::json &PluginManager::loadedAppSettings(const std::string &pluginName) {
    auto it = appSettingsCache_.find(pluginName);
    if (it != appSettingsCache_.end())
        return it->second;
    nlohmann::json obj = nlohmann::json::object();
    if (auto text = ofs::util::readFile(appSettingsPath(pluginName))) {
        // A missing file is the normal first-run case; a corrupt or non-object file is treated as empty
        // (don't toast — the plugin just starts from defaults and overwrites it on its next save).
        auto parsed = nlohmann::json::parse(*text, nullptr, /*allow_exceptions=*/false);
        if (parsed.is_object())
            obj = std::move(parsed);
    }
    return appSettingsCache_.emplace(pluginName, std::move(obj)).first->second;
}

const nlohmann::json *PluginManager::appSettingValue(const std::string &pluginName, const std::string &key) {
    const nlohmann::json &obj = loadedAppSettings(pluginName);
    auto it = obj.find(key);
    if (it == obj.end() || it->is_null())
        return nullptr;
    return &*it;
}

void PluginManager::setAppSetting(const std::string &pluginName, const std::string &key, nlohmann::json value) {
    nlohmann::json &obj = loadedAppSettings(pluginName);
    if (value.is_null()) {
        if (obj.erase(key) == 0)
            return;
    } else {
        obj[key] = std::move(value);
    }
    appSettingsDirty_.insert(pluginName);
}

void PluginManager::flushDirtyAppSettings() {
    for (const auto &name : appSettingsDirty_) {
        auto it = appSettingsCache_.find(name);
        if (it == appSettingsCache_.end())
            continue;
        const std::filesystem::path path = appSettingsPath(name);
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        if (!ofs::util::writeFileAtomic(path, it->second.dump(2)))
            OFS_CORE_ERROR("Failed to write plugin settings for '{}': {}", name, ofs::util::toUtf8(path));
    }
    appSettingsDirty_.clear();
}

void PluginManager::update(float delta) {
    // Drain ran first this frame, so every AxisModifiedEvent for the frame is now recorded. Fire the
    // coalesced per-axis callbacks before onTimeChange/onUpdate, preserving the old ordering where the
    // axis-modified notification (then dispatched directly from the handler during drain) preceded them.
    flushAxisModified();

    // Developer-workflow polls, throttled to a shared cadence and run here (in update(), before
    // onImGuiRender) so any plugin load/reload swaps PluginApi pointers before this frame's callbacks
    // and never mid-onBuildUI: (1) surface a plugin folder that appeared since launch, and (2) hot-reload
    // any plugin whose toggle is on when its DLL changes.
    pluginPollAccum_ += delta;
    constexpr float kPluginPollInterval = 1.0f; // 1 s latency is fine for a dev rebuild; keeps the scan cheap
    if (pluginPollAccum_ >= kPluginPollInterval) {
        pluginPollAccum_ = 0.f;
        discoverNewPlugins();
        pollHotReload();
    }

    double currentTime = project_.playback.cursorPos;
    if (std::abs(currentTime - lastReportedTime_) > 0.001) {
        lastReportedTime_ = currentTime;
        for (auto &plugin : loadedPlugins) {
            if (!plugin.enabled)
                continue;
            if (plugin.api.onTimeChange) {
                CurrentPluginScope scope(callCtx_, plugin.name.c_str());
                plugin.api.onTimeChange(currentTime);
            }
        }
    }

    for (auto &plugin : loadedPlugins) {
        if (!plugin.enabled)
            continue;
        if (plugin.api.onUpdate) {
            CurrentPluginScope scope(callCtx_, plugin.name.c_str());
            plugin.api.onUpdate(delta);
        }
    }

    // Persist any app-scoped settings a plugin's OnUpdate (its AppScoped flush) changed this frame —
    // debounced to one disk write per dirtied plugin per frame, same model as the host's own AppSettings.
    flushDirtyAppSettings();
}

void PluginManager::renderUI() {
    for (auto &plugin : loadedPlugins) {
        if (!plugin.enabled || !plugin.api.onBuildUI)
            continue;
        if (!plugin.windowOpen)
            continue;

        bool wasOpen = plugin.windowOpen;
        // Font-relative so the default size and bounds scale with DPI / font, and a longer translated
        // plugin window title isn't clipped by a fixed pixel width.
        const float fs = ImGui::GetFontSize();
        ImGui::SetNextWindowSize(ImVec2(fs * 22.5f, fs * 30.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSizeConstraints({fs * 12.5f, fs * 6.25f}, {fs * 75.0f, fs * 75.0f});
        // "###" (not "##") so the ImGui id hashes only the DLL stem (plugin.name) — a plugin that
        // localizes or changes its display name keeps the same window/dock id (DockLayout binds the
        // same "…###<name>" slug). With "##" the id would fold in the visible display name and break.
        // NoNavInputs: plugin-defined widgets may expect raw arrow/Enter keys, so keep ImGui keyboard
        // nav out of plugin windows (consistent with the editor's own panels — see Application.cpp).
        if (ImGui::Begin(fmtScratch("{}###{}", plugin.displayName, plugin.name), &plugin.windowOpen,
                         ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                             ImGuiWindowFlags_NoNavInputs)) {
            callCtx_.currentPluginName = plugin.name.c_str();
            callCtx_.uiIdCounter = 0;
            callCtx_.uiSectionDepth = 0;
            callCtx_.inRow = false;
            // The plugin widgets live in this child, and window nav flags don't inherit — so the parent's
            // NoNavInputs doesn't reach here. Without it, clicking the content focuses the child and raises
            // io.NavActive/WantCaptureKeyboard, letting ImGui keyboard nav grab focus (and the BindingSystem
            // gate swallow shortcuts). Pass NoNavInputs to the child too (same fix as the imnodes canvas in
            // ProcessingPanel).
            ImGui::BeginChild("##plugin_content", {0.f, 0.f}, 0, ImGuiWindowFlags_NoNavInputs);
            plugin.api.onBuildUI();
            ImGui::EndChild();
            callCtx_.currentPluginName = nullptr;
        }
        ImGui::End();
        if (wasOpen && !plugin.windowOpen)
            savePluginStates();
    }
}

void PluginManager::renderIntentUi(const std::string &owningPlugin, OfsIntentUiFn onUi, void *userData) {
    if (!onUi)
        return;
    // Mirror the per-call setup renderUI() does around onBuildUI so the ui* builder keys ids off a fresh
    // counter and attributes project-data/log calls to the owning plugin. The caller already opened the
    // popup/window this draws into.
    callCtx_.currentPluginName = owningPlugin.c_str();
    callCtx_.uiIdCounter = 0;
    callCtx_.uiSectionDepth = 0;
    callCtx_.inRow = false;
    onUi(userData);
    callCtx_.currentPluginName = nullptr;
}

void PluginManager::firePluginCommand(const std::string &pluginName, const std::string &commandId) {
    for (auto &plugin : loadedPlugins) {
        if (plugin.name == pluginName && plugin.enabled && plugin.api.onCommand) {
            CurrentPluginScope scope(callCtx_, plugin.name.c_str());
            plugin.api.onCommand(commandId.c_str());
        }
    }
}

void PluginManager::onPlayStateChanged(const PlayStateChangedEvent &e) {
    for (auto &plugin : loadedPlugins) {
        if (!plugin.enabled)
            continue;
        if (plugin.api.onPlayChange) {
            CurrentPluginScope scope(callCtx_, plugin.name.c_str());
            plugin.api.onPlayChange(e.playing ? 1 : 0);
        }
    }
}

void PluginManager::onSetLanguage(const SetLanguageEvent &) {
    // The host renders plugin-supplied strings verbatim — it does not translate them. Each plugin reads
    // the active language at onLoad (getActiveLanguage) and registers its command titles, node display
    // names and window title in that language. Rather than a live re-supply path, we reload every plugin
    // so its onLoad re-runs and re-registers everything in the now-active language. Language switches are
    // rare, and plugins are collectible ALCs built to unload, so this reuses the hot-reload teardown.
    // OfsApp's SetLanguageEvent handler (which calls Translator::load) is registered before this one, and
    // EventQueue preserves registration order, so getActiveLanguage already returns the new code here.
    //
    // Tear down now, but defer the doLoad: tearDownPlugin pushes UnregisterPlugin* events that clear the
    // old command/node registrations, and command registration is synchronous — so re-loading here would
    // collide with the still-present old ids. The ReloadPluginsForLanguageEvent is pushed last, so it
    // drains after those unregisters (same pass) and doLoad then re-registers cleanly. A torn-down plugin
    // stays enabled but has a zeroed api (onUpdate/onBuildUI null-checked everywhere) until the reload.
    bool any = false;
    for (auto &lp : loadedPlugins) {
        if (!lp.enabled)
            continue;
        tearDownPlugin(lp);
        any = true;
    }
    if (any)
        eventQueue_.push(ReloadPluginsForLanguageEvent{});
}

void PluginManager::onReloadPluginsForLanguage(const ReloadPluginsForLanguageEvent &) {
    // Reload exactly the plugins onSetLanguage tore down: still enabled, but api zeroed (onLoad null).
    for (auto &lp : loadedPlugins) {
        if (!lp.enabled || lp.api.onLoad != nullptr)
            continue;
        eventQueue_.push(LoadShortcutBindingsEvent{});
        if (!doLoad(lp))
            lp.enabled = false;
    }
}

void PluginManager::onSpeedChanged(const SpeedChangedEvent &e) {
    for (auto &plugin : loadedPlugins) {
        if (!plugin.enabled)
            continue;
        if (plugin.api.onSpeedChange) {
            CurrentPluginScope scope(callCtx_, plugin.name.c_str());
            plugin.api.onSpeedChange(e.speed);
        }
    }
}

void PluginManager::onMediaChanged(const MediaChangedEvent &e) {
    for (auto &plugin : loadedPlugins) {
        if (!plugin.enabled)
            continue;
        if (plugin.api.onMediaChange) {
            CurrentPluginScope scope(callCtx_, plugin.name.c_str());
            plugin.api.onMediaChange(e.path.c_str());
        }
    }
}

void PluginManager::onProjectLoaded(const LoadProjectEvent &) {
    lastReportedTime_ = -1.0;
    // Drop any deferred Node.Update queued against the old project — its node ids are about to be reused by
    // the freshly loaded graph, so a stale mutation must not survive to apply to a different node.
    if (effectReg_.nodeStateCodec.clearNodeUpdates)
        effectReg_.nodeStateCodec.clearNodeUpdates();
    for (auto &plugin : loadedPlugins) {
        if (!plugin.enabled)
            continue;
        if (plugin.api.onProjectChange) {
            CurrentPluginScope scope(callCtx_, plugin.name.c_str());
            plugin.api.onProjectChange();
        }
    }
}

void PluginManager::onAxisModified(const AxisModifiedEvent &e) {
    // Just mark the role dirty; flushAxisModified() fires the plugin callbacks once per frame from
    // update(). Existence is re-checked at flush time, so a role that briefly toggled this frame
    // reflects its final state.
    axisModifiedDirty_.set(static_cast<size_t>(e.role));
}

void PluginManager::flushAxisModified() {
    if (axisModifiedDirty_.none())
        return;
    for (size_t roleIdx = 0; roleIdx < kStandardAxisCount; ++roleIdx) {
        if (!axisModifiedDirty_.test(roleIdx))
            continue;
        if (!axisExists(project_, roleIdx))
            continue;
        for (auto &plugin : loadedPlugins) {
            if (!plugin.enabled)
                continue;
            if (plugin.api.onAxisModified) {
                CurrentPluginScope scope(callCtx_, plugin.name.c_str());
                plugin.api.onAxisModified(static_cast<int>(roleIdx));
            }
        }
    }
    axisModifiedDirty_.reset();
}

void PluginManager::onAxisSelected(const AxisSelectedEvent &e) {
    const int role = static_cast<int>(e.role);
    for (auto &plugin : loadedPlugins) {
        if (!plugin.enabled)
            continue;
        if (plugin.api.onActiveAxisChanged) {
            CurrentPluginScope scope(callCtx_, plugin.name.c_str());
            plugin.api.onActiveAxisChanged(role);
        }
    }
}

void PluginManager::onSetPluginEnabled(const SetPluginEnabledEvent &e) {
    if (e.enabled)
        enablePluginWithConsent(e.name); // may show the trust modal before loading
    else
        setPluginEnabled(e.name, false);
}

void PluginManager::onSavePluginStates(const SavePluginStatesEvent &) const {
    savePluginStates();
}

void PluginManager::onRegisterPluginNode(const RegisterPluginNodeEvent &e) {
    if (!effectReg_.pluginNodes.contains(e.entry.id))
        effectReg_.pluginNodeKeys.push_back(e.entry.id);
    effectReg_.pluginNodes[e.entry.id] = e.entry;
}

void PluginManager::onUnregisterPluginNodes(const UnregisterPluginNodesEvent &e) {
    const std::string prefix = fmt::format("{}.", e.pluginName);
    std::erase_if(effectReg_.pluginNodes, [&prefix](const auto &kv) { return kv.first.starts_with(prefix); });
    std::erase_if(effectReg_.pluginNodeKeys, [&prefix](const std::string &k) { return k.starts_with(prefix); });
}

} // namespace ofs
