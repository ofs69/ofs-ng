#include "Core/Events.h"
#include "Format/AppSettings.h"
#include "Services/BindingSystem.h"
#include "Util/FileUtil.h"
#include "Util/PathUtil.h"
#include "helpers/TestState.h"
#include <SDL3/SDL_keycode.h>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <imgui.h>
#include <imgui_internal.h> // ImGuiItemFlags_Disabled, ImGuiWindow
#include <imgui_te_context.h>
#include <imgui_te_engine.h>
#include <string>
#include <variant>

// UI tests for the Shortcut Bindings window (src/UI/ShortcutWindow.cpp): filtering, the "Only bound"
// toggle, Reset to Defaults, setting/removing a keybind, and the preset save/load/delete cycle. They
// drive the live app exactly as a user would (menu → window → buttons/modals) and assert against the
// real BindingSystem state, never against rendered text. Every element is addressed by its stable
// ###id (added in ShortcutWindow.cpp where one was missing).

using namespace ofs;

namespace {

// The window's ### id, absolute so it ignores the current SetRef.
constexpr const char *kWin = "//Shortcut Bindings###shortcut_bindings";
constexpr const char *kMenu = "//##MainMenuBar/###menu_view/###menu_shortcuts";

// BeginPopupModal pushes onto OpenPopupStack, so this reports whether any modal is up without touching
// the current window — safe between frames (mirrors suite_modals / suite_command_palette).
bool anyModalOpen() {
    return ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
}

// True if `commandId` currently has a keyboard binding to `key` (modifiers ignored).
bool hasKeyBinding(const char *commandId, SDL_Keycode key) {
    for (const auto &b : getTestState().bindingSystem->bindings()) {
        if (b.commandId != commandId)
            continue;
        if (const auto *kc = std::get_if<KeyChord>(&b.trigger); kc != nullptr && kc->key == key)
            return true;
    }
    return false;
}

// True if `commandId` has a binding to this exact keyboard chord (key + modifiers).
bool hasChord(const char *commandId, SDL_Keycode key, SDL_Keymod mods) {
    for (const auto &b : getTestState().bindingSystem->bindings()) {
        if (b.commandId != commandId)
            continue;
        if (const auto *kc = std::get_if<KeyChord>(&b.trigger);
            kc != nullptr && kc->key == key && kc->modifiers == mods)
            return true;
    }
    return false;
}

// How many bindings (across all commands) hold this exact chord. The window's invariant is "a trigger
// maps to at most one command", so this must never exceed 1.
int countChord(SDL_Keycode key, SDL_Keymod mods) {
    int n = 0;
    for (const auto &b : getTestState().bindingSystem->bindings())
        if (const auto *kc = std::get_if<KeyChord>(&b.trigger);
            kc != nullptr && kc->key == key && kc->modifiers == mods)
            ++n;
    return n;
}

// Activation mode of the binding for `commandId` on `key`. Caller guards existence with hasKeyBinding.
ActivationMode bindingMode(const char *commandId, SDL_Keycode key) {
    for (const auto &b : getTestState().bindingSystem->bindings())
        if (b.commandId == commandId)
            if (const auto *kc = std::get_if<KeyChord>(&b.trigger); kc != nullptr && kc->key == key)
                return b.mode;
    return ActivationMode::Press;
}

// Id of the registered Custom command with this display title, or "" if none. Custom ids are
// "custom.<n>" assigned by the store, so a test reads the id back by the unique name it gave.
std::string customCmdIdByTitle(const char *title) {
    for (const auto &c : getTestState().commandRegistry->all())
        if (c.source == CommandSource::Custom && c.title == title)
            return c.id;
    return "";
}

bool presetListed(const std::string &slug) {
    for (const auto &p : getTestState().bindingSystem->listPresets())
        if (p.slug == slug)
            return true;
    return false;
}

// The View ▸ Keyboard Shortcuts menu item is a toggle, and a prior suite may leave it open/closed.
// Guard with the NoError flag — without it a missing-window probe flags the context error state and
// silently skips all later ctx ops (see reference_ui_test_engine_refs).
void openWindow(ImGuiTestContext *ctx) {
    if (ctx->WindowInfo(kWin, ImGuiTestOpFlags_NoError).Window == nullptr) {
        ctx->MenuClick(kMenu);
        ctx->Yield(2);
    }
    ctx->SetRef(kWin);
}

void closeWindow(ImGuiTestContext *ctx) {
    if (ctx->WindowInfo(kWin, ImGuiTestOpFlags_NoError).Window != nullptr) {
        ctx->MenuClick(kMenu);
        ctx->Yield(2);
    }
}

// Force a fresh open so the cached preset list (m_presetsLoaded) is rebuilt from disk.
void reopenWindow(ImGuiTestContext *ctx) {
    closeWindow(ctx);
    openWindow(ctx);
}

// Set the filter box (ref must already be kWin). Empty string clears it.
void setFilter(ImGuiTestContext *ctx, const char *text) {
    ctx->ItemClick("###scfilter");
    ctx->KeyCharsReplace(text);
    ctx->Yield(2);
}

// Known control state: filter cleared, "Only bound" off. ItemUncheck is a no-op when already off.
void resetControls(ImGuiTestContext *ctx) {
    ctx->ItemUncheck("###onlybound");
    setFilter(ctx, "");
}

// Click a button inside the currently focused modal popup, then restore the ref to the window.
void clickInModal(ImGuiTestContext *ctx, const char *buttonId) {
    ctx->SetRef("//$FOCUSED");
    ctx->ItemClick(buttonId);
    ctx->Yield(3);
    ctx->SetRef(kWin);
}

// The command list is a BeginChild whose window name carries a runtime hash suffix, so WindowInfo
// can't seed it (see reference_ui_test_engine_refs); resolve it by its structural "##shortcut_list"
// id. Match on that id alone — never on the "<parent>" portion of the name, which is the window's
// translated title and shifts per language.
ImGuiWindow *listChild() {
    for (ImGuiWindow *w : ImGui::GetCurrentContext()->Windows)
        if (std::strstr(w->Name, "##shortcut_list") != nullptr)
            return w;
    return nullptr;
}

// Capture a keyboard chord onto the single command currently isolated by the filter: open its capture
// modal via "+", inject the key the user would press (the KeyDownEvent the app pushes from SDL), and
// Apply. Ref must already be kWin and exactly one "+" must be visible.
void captureChordForVisibleCommand(ImGuiTestContext *ctx, SDL_Keycode key, SDL_Keymod mods) {
    ctx->ItemClick("**/###bindadd");
    ctx->Yield(3);
    IM_CHECK(anyModalOpen());
    getTestState().eventQueue->push(KeyDownEvent{.key = key, .modifiers = mods, .repeat = false});
    ctx->Yield(3);
    clickInModal(ctx, "###captureapply");
    IM_CHECK(!anyModalOpen());
}

} // namespace

void RegisterShortcutTests(ImGuiTestEngine *e) {
    // ── Filtering: title, group, and a no-match query ─────────────────────────────
    // A group header (###grp_<name>) renders only when at least one of its commands matches, so group
    // presence/absence is a clean observable for "did the filter narrow the list".
    IM_REGISTER_TEST(e, "shortcuts", "filter_narrows_list")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        getTestState().bindingSystem->resetToDefaults();
        openWindow(ctx);
        resetControls(ctx);

        // Title match: "Save Project" (core.save) surfaces only the Core group.
        setFilter(ctx, localizedCommandTitle("core.save").c_str());
        IM_CHECK(ctx->ItemExists("**/###grp_Core"));
        IM_CHECK(!ctx->ItemExists("**/###grp_Moving"));

        // Group-name match: the localized "Moving" group name surfaces the Moving group.
        setFilter(ctx, localizedGroupName("Moving").c_str());
        IM_CHECK(ctx->ItemExists("**/###grp_Moving"));

        // A query that matches nothing collapses the list entirely — no group headers.
        setFilter(ctx, "zzqqxx");
        IM_CHECK(!ctx->ItemExists("**/###grp_Core"));
        IM_CHECK(!ctx->ItemExists("**/###grp_Moving"));

        resetControls(ctx);
        closeWindow(ctx);
    };

    // ── "Only bound" hides commands with no binding ───────────────────────────────
    IM_REGISTER_TEST(e, "shortcuts", "only_bound_hides_unbound")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        getTestState().bindingSystem->resetToDefaults();
        openWindow(ctx);
        resetControls(ctx);

        // player.goto-start ("Go to Start") is bindable but unbound by default. With "Only bound" OFF
        // its row (and the Player group) is visible.
        setFilter(ctx, localizedCommandTitle("player.goto-start").c_str());
        IM_CHECK(ctx->ItemExists("**/###grp_Player"));

        // Turning "Only bound" ON drops the unbound command, emptying — and so hiding — its group.
        ctx->ItemCheck("###onlybound");
        ctx->Yield(2);
        IM_CHECK(!ctx->ItemExists("**/###grp_Player"));

        // A bound command stays visible under "Only bound": core.save keeps its Ctrl+S default.
        setFilter(ctx, localizedCommandTitle("core.save").c_str());
        IM_CHECK(ctx->ItemExists("**/###grp_Core"));

        resetControls(ctx);
        closeWindow(ctx);
    };

    // ── "Only bound" expands groups once but leaves them collapsible (regression: was forced open every
    //    frame, so a header could never be closed while the toggle was on) ──────────────────────────────
    IM_REGISTER_TEST(e, "shortcuts", "only_bound_groups_collapsible")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        getTestState().bindingSystem->resetToDefaults();
        openWindow(ctx);
        resetControls(ctx);

        // Checking "Only bound" expands the matching groups, so a bound command's row is visible: core.save
        // (Ctrl+S by default) sits in the Core group. (No filter — a filter force-opens every frame.)
        ctx->ItemCheck("###onlybound");
        ctx->Yield(2);
        IM_CHECK(ctx->ItemExists("**/core.save/###bindadd"));

        // The header must still collapse and stay collapsed — the expand is one-shot, not forced each frame.
        ctx->ItemClick("**/###grp_Core");
        ctx->Yield(2);
        IM_CHECK(!ctx->ItemExists("**/core.save/###bindadd"));

        resetControls(ctx);
        closeWindow(ctx);
    };

    // ── Custom command: create via the editor, bind it, delete it (binding pruned) ─────────────────
    IM_REGISTER_TEST(e, "shortcuts", "custom_command_create_bind_delete")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        getTestState().bindingSystem->resetToDefaults();
        openWindow(ctx);
        resetControls(ctx);

        // Open the Add-command picker and pick the "Step" kind directly — that opens the editor scoped to
        // Step. Name it "UICustomStep".
        ctx->ItemClick("###addcommand");
        ctx->Yield(2);
        ctx->SetRef("//$FOCUSED");
        ctx->ItemClick("###step");
        ctx->Yield(3);
        IM_CHECK(anyModalOpen());
        ctx->SetRef("//$FOCUSED");
        ctx->ItemClick("###custom_name");
        ctx->KeyCharsReplace("UICustomStep");
        ctx->ItemClick("###customsaveconfirm");
        ctx->Yield(3);
        ctx->SetRef(kWin);
        IM_CHECK(!anyModalOpen());

        // It registered as a rebind-listed, holdable Custom command.
        const std::string id = customCmdIdByTitle("UICustomStep");
        IM_CHECK(!id.empty());
        const Command *c = getTestState().commandRegistry->find(id);
        IM_CHECK(c != nullptr);
        IM_CHECK(c->source == CommandSource::Custom);
        IM_CHECK(c->inRebindList);
        IM_CHECK(c->holdable());

        // Isolate its row by the (unique) title and bind F8 to it through the capture modal.
        setFilter(ctx, "UICustomStep");
        captureChordForVisibleCommand(ctx, SDLK_F8, SDL_KMOD_NONE);
        IM_CHECK(hasKeyBinding(id.c_str(), SDLK_F8));

        // Delete it via the row's trash button + confirm. The binding is pruned by BindingSystem's own
        // handler for RemoveCustomCommandEvent.
        ctx->ItemClick("**/###cmddelete");
        ctx->Yield(3);
        IM_CHECK(anyModalOpen());
        clickInModal(ctx, "###customdeleteconfirm");
        IM_CHECK(!anyModalOpen());
        IM_CHECK(getTestState().commandRegistry->find(id) == nullptr);
        IM_CHECK(!hasKeyBinding(id.c_str(), SDLK_F8)); // binding pruned on delete

        resetControls(ctx);
        closeWindow(ctx);
    };

    // ── Add-command picker: a provider *category* → target modal → bind, which surfaces it in the list ──
    IM_REGISTER_TEST(e, "shortcuts", "add_provider_command_via_picker")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        getTestState().bindingSystem->resetToDefaults();
        openWindow(ctx);
        resetControls(ctx);

        // nav.axis.L0 ("Select Axis: …") is a registry-resident provider command: inRebindList=false, so it is
        // NOT in the main list until bound. It must exist (L0 always does) and start unbound.
        IM_CHECK(getTestState().commandRegistry->find("nav.axis.L0") != nullptr);
        IM_CHECK(!hasKeyBinding("nav.axis.L0", SDLK_F9));
        setFilter(ctx, "nav.axis.L0");
        IM_CHECK(!ctx->ItemExists("**/###bindadd")); // unbound provider: absent from the table
        setFilter(ctx, "");

        // Open the picker and choose the "Select Axis…" category — not a flat list of every axis. That opens
        // the target modal; pick L0 there, which starts the binding capture for nav.axis.L0.
        ctx->ItemClick("###addcommand");
        ctx->Yield(2);
        ctx->SetRef("//$FOCUSED");
        ctx->ItemClick("###addcat_Select Axis");
        ctx->Yield(3);
        IM_CHECK(anyModalOpen());
        ctx->SetRef("//$FOCUSED");
        ctx->ItemClick("**/###provpick_nav.axis.L0"); // target lives in the modal's "##provtargets" child
        ctx->Yield(3);
        IM_CHECK(anyModalOpen()); // the capture modal now

        // Capture F9 and apply — the same flow the table "+" uses.
        getTestState().eventQueue->push(KeyDownEvent{.key = SDLK_F9, .modifiers = SDL_KMOD_NONE, .repeat = false});
        ctx->Yield(3);
        clickInModal(ctx, "###captureapply");
        IM_CHECK(!anyModalOpen());
        IM_CHECK(hasKeyBinding("nav.axis.L0", SDLK_F9));

        // Now bound, the provider row appears in the main list (the hasValidBinding gate).
        setFilter(ctx, "nav.axis.L0");
        IM_CHECK(ctx->ItemExists("**/###bindadd"));

        // Remove the binding and it drops back out of the list.
        ctx->ItemClick("**/###bindremove");
        ctx->Yield(2);
        IM_CHECK(!hasKeyBinding("nav.axis.L0", SDLK_F9));
        IM_CHECK(!ctx->ItemExists("**/###bindadd"));

        resetControls(ctx);
        closeWindow(ctx);
    };

    // ── Reset to Defaults restores a removed binding ──────────────────────────────
    IM_REGISTER_TEST(e, "shortcuts", "reset_to_defaults")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &binding = *getTestState().bindingSystem;
        binding.resetToDefaults();
        IM_CHECK(hasKeyBinding("core.save", SDLK_S)); // Ctrl+S default

        // Strip the default so Reset has something observable to restore.
        binding.removeBinding(KeyChord{.key = SDLK_S, .modifiers = SDL_KMOD_CTRL}, "core.save");
        binding.saveBindings();
        IM_CHECK(!hasKeyBinding("core.save", SDLK_S));

        // Reset also restores the global input tunables (gamepad/analog + hold-repeat), so dirty them
        // first and confirm Reset puts them back to defaults.
        auto *s = getTestState().appSettings;
        IM_CHECK(s != nullptr);
        getTestState().eventQueue->push(ofs::ModifyEvent<ofs::AppSettings>{[](ofs::AppSettings &as) {
            as.input.deadzone = 0.40f;
            as.holdRepeat.accel = 0.85f;
        }});
        ctx->Yield(2);
        IM_CHECK_LT(std::abs(s->input.deadzone - 0.40f), 0.01f); // dirtied

        openWindow(ctx);
        resetControls(ctx);
        ctx->ItemClick("###screset");
        ctx->Yield(3);
        IM_CHECK(anyModalOpen());
        clickInModal(ctx, "###resetconfirm");
        ctx->Yield(2);
        IM_CHECK(!anyModalOpen());
        IM_CHECK(hasKeyBinding("core.save", SDLK_S)); // bindings restored
        IM_CHECK_LT(std::abs(s->input.deadzone - ofs::InputSettings{}.deadzone), 0.01f);
        IM_CHECK_LT(std::abs(s->holdRepeat.accel - ofs::HoldRepeatSettings{}.accel), 0.01f);

        closeWindow(ctx);
    };

    // ── Set a keybind through the capture modal ───────────────────────────────────
    // The "+" opens the capture modal; the key the user would press arrives as the KeyDownEvent the app
    // itself pushes from SDL — we inject it directly so the real BindingSystem capture path runs.
    IM_REGISTER_TEST(e, "shortcuts", "set_keybind_via_capture")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &binding = *getTestState().bindingSystem;
        binding.resetToDefaults();
        IM_CHECK(!hasKeyBinding("core.quick-export", SDLK_F9));

        openWindow(ctx);
        resetControls(ctx);
        setFilter(
            ctx,
            localizedCommandTitle("core.quick-export").c_str()); // isolate core.quick-export so its "+" is the only one

        ctx->ItemClick("**/###bindadd");
        ctx->Yield(3);
        IM_CHECK(anyModalOpen());

        getTestState().eventQueue->push(KeyDownEvent{.key = SDLK_F9, .modifiers = SDL_KMOD_NONE, .repeat = false});
        ctx->Yield(3); // capture resolves → modal shows the result + Apply

        clickInModal(ctx, "###captureapply");
        IM_CHECK(!anyModalOpen());
        IM_CHECK(hasKeyBinding("core.quick-export", SDLK_F9));

        resetControls(ctx);
        closeWindow(ctx);
    };

    // ── Remove a keybind with the trash button ────────────────────────────────────
    IM_REGISTER_TEST(e, "shortcuts", "remove_keybind")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &binding = *getTestState().bindingSystem;
        binding.resetToDefaults();
        IM_CHECK(hasKeyBinding("core.save", SDLK_S));

        openWindow(ctx);
        resetControls(ctx);
        setFilter(
            ctx,
            localizedCommandTitle("core.save").c_str()); // core.save has a single default binding → one trash button

        ctx->ItemClick("**/###bindremove");
        ctx->Yield(3);
        IM_CHECK(!hasKeyBinding("core.save", SDLK_S));

        resetControls(ctx);
        closeWindow(ctx);
    };

    // ── Assigning a trigger already in use reassigns it (conflict, no second owner) ─
    // Ctrl+S is core.save's default. Capturing it onto core.quick-export must move it — core.save
    // loses it, quick-export gains it, and exactly one command ends up holding Ctrl+S.
    IM_REGISTER_TEST(e, "shortcuts", "conflict_reassigns_trigger")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &binding = *getTestState().bindingSystem;
        binding.resetToDefaults();
        IM_CHECK(hasChord("core.save", SDLK_S, SDL_KMOD_CTRL));
        IM_CHECK(!hasChord("core.quick-export", SDLK_S, SDL_KMOD_CTRL));
        IM_CHECK_EQ(countChord(SDLK_S, SDL_KMOD_CTRL), 1);

        openWindow(ctx);
        resetControls(ctx);
        setFilter(ctx, localizedCommandTitle("core.quick-export").c_str()); // isolate core.quick-export
        captureChordForVisibleCommand(ctx, SDLK_S, SDL_KMOD_CTRL);

        IM_CHECK(hasChord("core.quick-export", SDLK_S, SDL_KMOD_CTRL)); // reassigned to the new command
        IM_CHECK(!hasChord("core.save", SDLK_S, SDL_KMOD_CTRL));        // taken away from the old owner
        IM_CHECK_EQ(countChord(SDLK_S, SDL_KMOD_CTRL), 1);              // never two owners of one trigger

        resetControls(ctx);
        closeWindow(ctx);
    };

    // ── Assigning the same trigger to the same command twice does not duplicate it ──
    IM_REGISTER_TEST(e, "shortcuts", "duplicate_binding_prevented")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &binding = *getTestState().bindingSystem;
        binding.resetToDefaults();

        openWindow(ctx);
        resetControls(ctx);
        setFilter(ctx, localizedCommandTitle("core.quick-export").c_str()); // isolate core.quick-export

        // Assign F9 once, then assign F9 again to the very same command.
        captureChordForVisibleCommand(ctx, SDLK_F9, SDL_KMOD_NONE);
        IM_CHECK_EQ(countChord(SDLK_F9, SDL_KMOD_NONE), 1);
        captureChordForVisibleCommand(ctx, SDLK_F9, SDL_KMOD_NONE);

        // Still exactly one F9 binding, still owned by quick-export — no duplicate row created.
        IM_CHECK(hasChord("core.quick-export", SDLK_F9, SDL_KMOD_NONE));
        IM_CHECK_EQ(countChord(SDLK_F9, SDL_KMOD_NONE), 1);

        resetControls(ctx);
        closeWindow(ctx);
    };

    // ── Save the active bindings as a preset, then load it back ────────────────────
    IM_REGISTER_TEST(e, "shortcuts", "preset_save_and_load")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &binding = *getTestState().bindingSystem;
        binding.resetToDefaults();
        // A distinctive binding so a later reload is observable.
        binding.addBinding(Binding{.trigger = KeyChord{.key = SDLK_F9}, .commandId = "core.save"});
        binding.saveBindings();
        IM_CHECK(hasKeyBinding("core.save", SDLK_F9));

        reopenWindow(ctx); // fresh preset list

        // Save As "UI Test Preset" (slug "ui-test-preset").
        ctx->ItemClick("###presetsaveas");
        ctx->Yield(3);
        IM_CHECK(anyModalOpen());
        ctx->SetRef("//$FOCUSED");
        ctx->ItemClick("###presetname");
        ctx->KeyCharsReplace("UI Test Preset");
        ctx->ItemClick("###presetsaveconfirm");
        ctx->Yield(3);
        ctx->SetRef(kWin);
        IM_CHECK(!anyModalOpen());
        IM_CHECK(presetListed("ui-test-preset"));

        // Wipe the distinctive binding from the active set so the load has to bring it back.
        binding.resetToDefaults();
        IM_CHECK(!hasKeyBinding("core.save", SDLK_F9));

        // Select the preset in the combo and Load it (with confirm). The option carries a stable
        // "###preset_opt_<slug>" id (the visible label/suffix is translated), so target it by slug.
        ctx->ComboClick("###presetcombo/preset_opt_ui-test-preset");
        ctx->Yield(2);
        ctx->SetRef(kWin);
        ctx->ItemClick("###presetload");
        ctx->Yield(3);
        IM_CHECK(anyModalOpen());
        clickInModal(ctx, "###loadconfirm");
        IM_CHECK(!anyModalOpen());
        IM_CHECK(hasKeyBinding("core.save", SDLK_F9)); // preset restored it

        closeWindow(ctx);
    };

    // ── Delete a user preset ──────────────────────────────────────────────────────
    IM_REGISTER_TEST(e, "shortcuts", "preset_delete")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &binding = *getTestState().bindingSystem;
        binding.resetToDefaults();
        binding.saveActiveAsPreset("UI Test Preset"); // guarantee the preset exists on disk
        IM_CHECK(presetListed("ui-test-preset"));

        reopenWindow(ctx); // rebuild the combo so the preset is selectable

        ctx->ComboClick("###presetcombo/preset_opt_ui-test-preset");
        ctx->Yield(2);
        ctx->SetRef(kWin);
        ctx->ItemClick("###presetdelete"); // enabled for a user preset
        ctx->Yield(3);
        IM_CHECK(anyModalOpen());
        clickInModal(ctx, "###deleteconfirm");
        IM_CHECK(!anyModalOpen());
        IM_CHECK(!presetListed("ui-test-preset")); // gone from disk

        closeWindow(ctx);
    };

    // ── The Press/Hold mode selector changes a binding's activation mode ───────────
    IM_REGISTER_TEST(e, "shortcuts", "mode_selector_toggles_activation")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &binding = *getTestState().bindingSystem;
        binding.resetToDefaults();
        // navigation.next-step is holdable and ships bound to Right in Hold mode.
        IM_CHECK(hasKeyBinding("navigation.next-step", SDLK_RIGHT));
        IM_CHECK(bindingMode("navigation.next-step", SDLK_RIGHT) == ActivationMode::Hold);

        openWindow(ctx);
        resetControls(ctx);
        setFilter(ctx, localizedCommandTitle("navigation.next-step")
                           .c_str()); // isolate navigation.next-step (single binding → one combo)

        // The mode Combo has an empty visible label, so the **/ wildcard (which matches by label) can't
        // find it — address it by exact decorated path. Its per-trigger scope is PushID(uid), and for an
        // unmodified chord uid == key, so SDLK_RIGHT shows up as a "$$<int>" segment.
        ImGuiWindow *child = listChild();
        IM_CHECK(child != nullptr);
        ctx->SetRef(child);
        const std::string combo =
            "Navigation/##bindings/navigation.next-step/$$" + std::to_string(static_cast<int>(SDLK_RIGHT)) + "/##mode";
        ctx->ItemClick(combo.c_str()); // opens the Press/Hold dropdown
        ctx->Yield(2);
        ctx->SetRef("//$FOCUSED");
        ctx->ItemClick("**/mode_press"); // option carries a stable ###id, so it survives translation
        ctx->Yield(2);
        ctx->SetRef(kWin);
        IM_CHECK(bindingMode("navigation.next-step", SDLK_RIGHT) == ActivationMode::Press);

        resetControls(ctx);
        closeWindow(ctx);
    };

    // ── Cancelling capture leaves the binding set untouched ───────────────────────
    IM_REGISTER_TEST(e, "shortcuts", "capture_cancel_adds_nothing")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &binding = *getTestState().bindingSystem;
        binding.resetToDefaults();
        IM_CHECK(!hasKeyBinding("core.quick-export", SDLK_F9));

        openWindow(ctx);
        resetControls(ctx);
        setFilter(ctx, localizedCommandTitle("core.quick-export").c_str());

        ctx->ItemClick("**/###bindadd");
        ctx->Yield(3);
        IM_CHECK(anyModalOpen());
        getTestState().eventQueue->push(KeyDownEvent{.key = SDLK_F9, .modifiers = SDL_KMOD_NONE, .repeat = false});
        ctx->Yield(3); // captured, awaiting Apply/Cancel
        clickInModal(ctx, "###capturecancel");
        IM_CHECK(!anyModalOpen());
        IM_CHECK(!hasKeyBinding("core.quick-export", SDLK_F9)); // Cancel committed nothing

        resetControls(ctx);
        closeWindow(ctx);
    };

    // ── The filter also matches a binding's formatted trigger text ────────────────
    IM_REGISTER_TEST(e, "shortcuts", "filter_matches_trigger_text")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        getTestState().bindingSystem->resetToDefaults();
        openWindow(ctx);
        resetControls(ctx);

        // "Ctrl+Shift+S" is core.quick-export's default chord — it appears in no title/group/id, so a
        // match here exercises the trigger-text branch of the filter. It surfaces Core, not Navigation.
        setFilter(ctx, "Ctrl+Shift+S");
        IM_CHECK(ctx->ItemExists("**/###grp_Core"));
        IM_CHECK(!ctx->ItemExists("**/###grp_Navigation"));

        resetControls(ctx);
        closeWindow(ctx);
    };

    // ── A built-in preset can be loaded but not deleted ───────────────────────────
    IM_REGISTER_TEST(e, "shortcuts", "builtin_preset_delete_disabled")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        getTestState().bindingSystem->resetToDefaults();

        // Built-in presets ship read-only under the install data dir. Plant one so the combo lists it.
        namespace fs = std::filesystem;
        const fs::path dir = ofs::util::getBasePath() / "data" / "binding_presets";
        fs::create_directories(dir);
        const fs::path file = dir / "ui-builtin-test.json";
        ofs::util::writeFile(file, R"({"version":1,"name":"UI Builtin Test","bindings":[]})");

        reopenWindow(ctx); // rebuild the cached preset list from disk
        IM_CHECK(presetListed("ui-builtin-test"));

        ctx->ComboClick("###presetcombo/preset_opt_ui-builtin-test");
        ctx->Yield(2);
        ctx->SetRef(kWin);

        // Delete is disabled for a built-in; Load stays enabled.
        IM_CHECK((ctx->ItemInfo("###presetdelete").ItemFlags & ImGuiItemFlags_Disabled) != 0);
        IM_CHECK((ctx->ItemInfo("###presetload").ItemFlags & ImGuiItemFlags_Disabled) == 0);

        fs::remove(file); // clean up the planted built-in
        closeWindow(ctx);
    };

    // ── The Gamepad/Analog sliders write the AppSettings input tunables ────────────────
    // The collapsing header is collapsed by default; expand it, drag a slider, and assert the live
    // AppSettings.input value (read through OfsAppTestAccess::appSettings). Restored afterwards so the
    // global tunables don't leak into later suites.
    IM_REGISTER_TEST(e, "shortcuts", "gamepad_analog_sliders")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        getTestState().bindingSystem->resetToDefaults();
        const auto *s = getTestState().appSettings;
        IM_CHECK(s != nullptr);
        const float deadBefore = s->input.deadzone;
        const float smoothBefore = s->input.smoothing;

        openWindow(ctx);
        ctx->ItemClick("**/###gamepad_analog"); // expand the section
        ctx->Yield(2);

        ctx->ItemInputValue("**/###deadzone", 0.30f);
        ctx->Yield(2);
        IM_CHECK_LT(std::abs(s->input.deadzone - 0.30f), 0.02f);

        ctx->ItemInputValue("**/###smoothing", 0.20f);
        ctx->Yield(2);
        IM_CHECK_LT(std::abs(s->input.smoothing - 0.20f), 0.02f);

        // Restore the global defaults through the event queue (simplest robust reset).
        getTestState().eventQueue->push(
            ofs::ModifyEvent<ofs::AppSettings>{[deadBefore, smoothBefore](ofs::AppSettings &as) {
                as.input.deadzone = deadBefore;
                as.input.smoothing = smoothBefore;
            }});
        ctx->Yield(2);
        ctx->ItemClick("**/###gamepad_analog"); // re-collapse for the next suite
        ctx->Yield();
        closeWindow(ctx);
    };

    // ── The Hold Repeat sliders write the global hold-repeat cadence ───────────────────
    IM_REGISTER_TEST(e, "shortcuts", "hold_repeat_sliders")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        getTestState().bindingSystem->resetToDefaults();
        const auto *s = getTestState().appSettings;
        IM_CHECK(s != nullptr);
        const float delayBefore = s->holdRepeat.initialDelay;
        const float intervalBefore = s->holdRepeat.interval;
        const float accelBefore = s->holdRepeat.accel;

        openWindow(ctx);
        ctx->ItemClick("**/###hold_repeat"); // expand the section
        ctx->Yield(2);

        ctx->ItemInputValue("**/###hold_delay", 0.50f);
        ctx->Yield(2);
        IM_CHECK_LT(std::abs(s->holdRepeat.initialDelay - 0.50f), 0.02f);

        // Speed slider is a rate (repeats/sec); 10 /s ⇒ a 0.10 s interval.
        ctx->ItemInputValue("**/###hold_speed", 10.0f);
        ctx->Yield(2);
        IM_CHECK_LT(std::abs(s->holdRepeat.interval - 0.10f), 0.02f);

        // Acceleration slider is 0–100 %; 66.7 % maps to the stored factor ≈ 0.90.
        ctx->ItemInputValue("**/###hold_accel", 66.7f);
        ctx->Yield(2);
        IM_CHECK_LT(std::abs(s->holdRepeat.accel - 0.90f), 0.02f);

        // Restore the global defaults so the cadence doesn't leak into later suites.
        getTestState().eventQueue->push(
            ofs::ModifyEvent<ofs::AppSettings>{[delayBefore, intervalBefore, accelBefore](ofs::AppSettings &as) {
                as.holdRepeat.initialDelay = delayBefore;
                as.holdRepeat.interval = intervalBefore;
                as.holdRepeat.accel = accelBefore;
            }});
        ctx->Yield(2);
        ctx->ItemClick("**/###hold_repeat"); // re-collapse for the next suite
        ctx->Yield();
        closeWindow(ctx);
    };
}
