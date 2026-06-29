#include "Format/LayoutStore.h"
#include "UI/DockLayout.h" // ofs::ui::applyDefaultLayout
#include "helpers/OfsAppTestAccess.h"
#include "helpers/TestState.h"
#include <imgui.h>
#include <imgui_internal.h> // ImGuiWindow::DockNode, ImGuiDockNode, DockBuilder*
#include <imgui_te_context.h>
#include <imgui_te_engine.h>

// UI tests for the docking-layout system (Layout menu).
//
// The valuable behavior is the round-trip: a real arrangement change (redock / resize) is captured
// on "Save Layout" and faithfully restored when the layout is re-selected or "Reset Layout" is used.
// Co-location of two windows in one dock node (node IDs equal) is the observable we assert on — it
// survives node-ID renumbering across rebuilds, unlike absolute IDs. Persistence is cross-checked by
// reading layouts.json back via ofs::LayoutStore::load() (the pref path is a temp dir in tests).
//
// Tests are order-independent: each builds its own preconditions, and PopupCloseAll() clears any
// popup leaked by a previous suite. The single app instance's layoutStore accumulates across tests,
// so assertions never assume a pristine starting layout set.

static bool anyPopupOpen() {
    return ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
}

static ImGuiDockNode *dockedNode(ImGuiTestContext *ctx, const char *window) {
    ImGuiWindow *w = ctx->WindowInfo(window).Window;
    return w != nullptr ? w->DockNode : nullptr;
}

// The dock node whose *width* is the column width containing `window`. The right column stacks several
// windows with vertical (Y-axis) splits, so a leaf's immediate parent is a Y-split (resizing it moves
// a horizontal divider — height, not width). Walk up to the node that was split off horizontally (its
// parent splits on the X axis); that node spans the column's full width. Returns null if undocked.
static ImGuiDockNode *columnNode(ImGuiTestContext *ctx, const char *window) {
    ImGuiDockNode *n = dockedNode(ctx, window);
    while (n != nullptr && n->ParentNode != nullptr && n->ParentNode->SplitAxis != ImGuiAxis_X)
        n = n->ParentNode;
    return n;
}

// Two windows share one dock node (i.e. tabbed together)?
static bool coLocated(ImGuiTestContext *ctx, const char *a, const char *b) {
    ImGuiDockNode *na = dockedNode(ctx, a);
    ImGuiDockNode *nb = dockedNode(ctx, b);
    return na != nullptr && nb != nullptr && na->ID == nb->ID;
}

// The dockspace runs with AutoHideTabBar, so a node holding a single window hides its tab bar. DockInto
// drags a window by its tab, which then can't be located — so reveal the tab first (the same toggle the
// corner triangle performs) before a drag-redock. No-op once the node has a visible tab bar.
static void revealTabBar(ImGuiTestContext *ctx, const char *window) {
    if (ImGuiDockNode *n = dockedNode(ctx, window); n != nullptr && n->IsHiddenTabBar()) {
        n->WantHiddenTabBarToggle = true;
        ctx->Yield(2);
    }
}

// Reset to the built-in Default arrangement — a deterministic baseline independent of whatever a
// previous test left active (layout state persists across tests in one run).
static void selectDefaultLayout(ImGuiTestContext *ctx) {
    ctx->MenuClick("//##MainMenuBar/###menu_layout/###layout_default");
    ctx->Yield(3);
}

// Drive the "+ New Layout..." modal to create and activate a layout named `name`.
static void createLayout(ImGuiTestContext *ctx, const char *name) {
    ctx->MenuClick("//##MainMenuBar/###menu_layout/###NewLayout");
    ctx->Yield(3);
    IM_CHECK(anyPopupOpen());
    ctx->ItemClick("**/##layoutname"); // focus the name field (also auto-focused by the body)
    ctx->KeyCharsReplace(name);        // yields per char → value committed, Create enabled
    ctx->ItemClick("**/###newlayout_create");
    ctx->Yield(3);
    IM_CHECK(!anyPopupOpen());
}

void RegisterLayoutTests(ImGuiTestEngine *e) {
    // "+ New Layout..." creates a persisted layout and makes it active.
    IM_REGISTER_TEST(e, "layouts", "new_layout_creates_and_activates")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        createLayout(ctx, "UiTestCreate");

        const ofs::LayoutStore s = ofs::LayoutStore::load();
        IM_CHECK(s.activeLayoutName == "UiTestCreate");
        bool found = false;
        for (const auto &p : s.layouts)
            if (p.name == "UiTestCreate")
                found = true;
        IM_CHECK(found);
    };

    // Cancelling the modal persists no new layout.
    IM_REGISTER_TEST(e, "layouts", "new_layout_cancel_adds_nothing")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        const size_t before = ofs::LayoutStore::load().layouts.size();

        ctx->MenuClick("//##MainMenuBar/###menu_layout/###NewLayout");
        ctx->Yield(3);
        IM_CHECK(anyPopupOpen());
        ctx->ItemClick("**/###newlayout_cancel");
        ctx->Yield(3);
        IM_CHECK(!anyPopupOpen());

        IM_CHECK_EQ(ofs::LayoutStore::load().layouts.size(), before);
    };

    // Redock → Save → switch to Default → switch back: the saved arrangement is restored from the
    // persisted ini. This is the core save/load round-trip for an actual dock-tree change.
    IM_REGISTER_TEST(e, "layouts", "redock_saved_and_restored")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        selectDefaultLayout(ctx);
        createLayout(ctx, "RedockRT"); // active layout to save into (Default is read-only)

        // In the built-in arrangement these two live in different nodes.
        IM_CHECK(!coLocated(ctx, "Video Controls###video_controls", "Statistics###statistics"));

        // Real redock: drop Video Controls as a tab into the Statistics node.
        revealTabBar(ctx, "Video Controls###video_controls");
        ctx->DockInto("Video Controls###video_controls", "Statistics###statistics");
        ctx->Yield(3);
        IM_CHECK(coLocated(ctx, "Video Controls###video_controls", "Statistics###statistics"));

        ctx->MenuClick("//##MainMenuBar/###menu_layout/###SaveLayout");
        ctx->Yield(2);

        // Switch to the built-in Default — the windows split back apart.
        ctx->MenuClick("//##MainMenuBar/###menu_layout/###layout_default");
        ctx->Yield(3);
        IM_CHECK(ofs::LayoutStore::load().activeLayoutName == "Default");
        IM_CHECK(!coLocated(ctx, "Video Controls###video_controls", "Statistics###statistics"));

        // Re-select the saved layout — the redocked arrangement returns.
        ctx->MenuClick("//##MainMenuBar/###menu_layout/RedockRT");
        ctx->Yield(3);
        IM_CHECK(ofs::LayoutStore::load().activeLayoutName == "RedockRT");
        IM_CHECK(coLocated(ctx, "Video Controls###video_controls", "Statistics###statistics"));
    };

    // Resize a dock node → Save → switch away → switch back: the node width is restored.
    IM_REGISTER_TEST(e, "layouts", "resize_saved_and_restored")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        selectDefaultLayout(ctx);
        createLayout(ctx, "ResizeRT");

        // Resize the right column that holds the Statistics window — its width is governed by the
        // horizontally-split column node, not Statistics' immediate (vertical-split) parent. Width is
        // read back from that same column node each time.
        const char *win = "Statistics###statistics";
        auto columnWidth = [&]() -> float {
            ImGuiDockNode *col = columnNode(ctx, win);
            return col != nullptr ? col->Size.x : -1.0f;
        };
        ImGuiDockNode *column = columnNode(ctx, win);
        IM_CHECK(column != nullptr); // fail the test cleanly instead of segfaulting if it isn't docked yet
        const float widthBefore = column->Size.x;

        ImGui::DockBuilderSetNodeSize(column->ID, ImVec2(widthBefore + 160.0f, column->Size.y));
        ImGui::DockBuilderFinish(ImGui::GetID("MyDockSpace"));
        ctx->Yield(3);
        const float widthResized = columnWidth();
        IM_CHECK_GT(widthResized, widthBefore + 60.0f);

        ctx->MenuClick("//##MainMenuBar/###menu_layout/###SaveLayout");
        ctx->Yield(2);

        ctx->MenuClick("//##MainMenuBar/###menu_layout/###layout_default");
        ctx->Yield(3);
        IM_CHECK_LT(columnWidth(), widthResized - 60.0f); // back to the default width

        ctx->MenuClick("//##MainMenuBar/###menu_layout/ResizeRT");
        ctx->Yield(3);
        IM_CHECK_GT(columnWidth(), widthResized - 60.0f); // restored wide
    };

    // Reset Layout reverts unsaved live tweaks back to the layout's saved baseline.
    IM_REGISTER_TEST(e, "layouts", "reset_reverts_unsaved_changes")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        selectDefaultLayout(ctx);
        createLayout(ctx, "ResetRT"); // saved baseline = the built-in arrangement

        IM_CHECK(!coLocated(ctx, "Video Controls###video_controls", "Statistics###statistics"));
        revealTabBar(ctx, "Video Controls###video_controls");
        ctx->DockInto("Video Controls###video_controls", "Statistics###statistics"); // unsaved live change
        ctx->Yield(3);
        IM_CHECK(coLocated(ctx, "Video Controls###video_controls", "Statistics###statistics"));

        ctx->MenuClick("//##MainMenuBar/###menu_layout/###ResetLayout"); // discard tweaks, reload saved baseline
        ctx->Yield(3);
        IM_CHECK(ofs::LayoutStore::load().activeLayoutName == "ResetRT");
        IM_CHECK(!coLocated(ctx, "Video Controls###video_controls", "Statistics###statistics"));
    };

    // Locking sets NoTabBar on every docked node (not just the root) and persists the lock; unlocking
    // clears it. Normalizes to unlocked first so prior tests can't influence the result.
    IM_REGISTER_TEST(e, "layouts", "lock_hides_tabbars_globally")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        const char *win = "Video Controls###video_controls";

        if (ImGuiDockNode *n = dockedNode(ctx, win); n != nullptr && (n->MergedFlags & ImGuiDockNodeFlags_NoTabBar)) {
            ctx->MenuClick("//##MainMenuBar/###menu_layout/###LockLayout");
            ctx->Yield(3);
        }
        ImGuiDockNode *node = dockedNode(ctx, win);
        IM_CHECK(node != nullptr);
        IM_CHECK((node->MergedFlags & ImGuiDockNodeFlags_NoTabBar) == 0);

        ctx->MenuClick("//##MainMenuBar/###menu_layout/###LockLayout"); // lock
        ctx->Yield(3);
        node = dockedNode(ctx, win);
        IM_CHECK(node != nullptr);
        IM_CHECK((node->MergedFlags & ImGuiDockNodeFlags_NoTabBar) != 0);
        IM_CHECK(ofs::LayoutStore::load().locked);

        ctx->MenuClick("//##MainMenuBar/###menu_layout/###LockLayout"); // unlock (leave a clean state)
        ctx->Yield(3);
        node = dockedNode(ctx, win);
        IM_CHECK(node != nullptr);
        IM_CHECK((node->MergedFlags & ImGuiDockNodeFlags_NoTabBar) == 0);
        IM_CHECK(!ofs::LayoutStore::load().locked);
    };

    // The built-in Default layout scales its fixed-content panels (right-column width, Video Controls
    // strip height) with the DPI content scale (style.FontScaleDpi) so text rows don't clip when display
    // scaling is raised. Build the default at 1x and 2x and assert those nodes grew. The viewport pixel
    // size is constant across DPI here, so a raw-pixel comparison isolates the ratio change.
    IM_REGISTER_TEST(e, "layouts", "default_layout_scales_fixed_panels_with_dpi")->TestFunc =
        [](ImGuiTestContext *ctx) {
            loadFixture(ctx);
            selectDefaultLayout(ctx);

            auto columnWidth = [&]() -> float {
                ImGuiDockNode *c = columnNode(ctx, "Statistics###statistics");
                return c != nullptr ? c->Size.x : -1.0f;
            };
            auto controlsHeight = [&]() -> float {
                ImGuiDockNode *n = dockedNode(ctx, "Video Controls###video_controls");
                return n != nullptr ? n->Size.y : -1.0f;
            };

            ImGuiStyle &style = ImGui::GetStyle();
            const float savedDpi = style.FontScaleDpi;

            // buildDefault reads FontScaleDpi at build time; drive it directly. The per-frame DPI sync
            // only rewrites the style when the display scale actually changes (it doesn't, in tests), so
            // this value survives the Yields below.
            style.FontScaleDpi = 1.0f;
            ofs::ui::applyDefaultLayout();
            ctx->Yield(3);
            const float colAt1x = columnWidth();
            const float ctrlAt1x = controlsHeight();
            IM_CHECK_GT(colAt1x, 0.0f);
            IM_CHECK_GT(ctrlAt1x, 0.0f);

            style.FontScaleDpi = 2.0f;
            ofs::ui::applyDefaultLayout();
            ctx->Yield(3);
            const float colAt2x = columnWidth();
            const float ctrlAt2x = controlsHeight();

            // Ratios double (0.2→0.4, 0.28→0.56, both under their clamps); 1.4x is a safe lower bound.
            IM_CHECK_GT(colAt2x, colAt1x * 1.4f);
            IM_CHECK_GT(ctrlAt2x, ctrlAt1x * 1.4f);

            // Restore the real content scale and rebuild so later tests start from a clean baseline.
            style.FontScaleDpi = savedDpi;
            ofs::ui::applyDefaultLayout();
            ctx->Yield(3);
        };

    // A runtime content-scale change re-applies the active layout's saved baseline at the new scale (via
    // the onDisplayScaleChanged hook → revertToActiveLayout → the deferred apply onUpdate consumes) so
    // panels track the new scale without a restart. The real SDL display-scale trigger can't be
    // simulated headless: drive the hook directly. FontScaleDpi drives the Default rebuild; the user
    // layout's node rescale keys off the window content scale (unchangeable headless), so the user case
    // here asserts the re-apply (discard of unsaved tweaks); the rescale math is covered separately.
    IM_REGISTER_TEST(e, "layouts", "runtime_scale_change_reapplies_active_layout")->TestFunc =
        [](ImGuiTestContext *ctx) {
            loadFixture(ctx);
            OfsApp &app = *getTestState().app;
            ImGuiStyle &style = ImGui::GetStyle();
            const float savedDpi = style.FontScaleDpi;

            auto columnWidth = [&]() -> float {
                ImGuiDockNode *c = columnNode(ctx, "Statistics###statistics");
                return c != nullptr ? c->Size.x : -1.0f;
            };

            // Default active: a scale change rebuilds it wider (buildDefault reads FontScaleDpi).
            selectDefaultLayout(ctx); // built at the test's 1x scale
            const float colBefore = columnWidth();
            IM_CHECK_GT(colBefore, 0.0f);

            style.FontScaleDpi = 2.0f;
            OfsAppTestAccess::dispatchDisplayScaleChanged(app, 2.0f);
            ctx->Yield(3); // onUpdate consumes the pending reset and rebuilds at 2x
            IM_CHECK_GT(columnWidth(), colBefore * 1.4f);

            style.FontScaleDpi = savedDpi;
            OfsAppTestAccess::dispatchDisplayScaleChanged(app, savedDpi);
            ctx->Yield(3);

            // User layout active: the hook re-applies its saved baseline, discarding an unsaved redock.
            createLayout(ctx, "DpiRuntimeUser"); // saved baseline: the built-in arrangement
            IM_CHECK(!coLocated(ctx, "Video Controls###video_controls", "Statistics###statistics"));
            revealTabBar(ctx, "Video Controls###video_controls");
            ctx->DockInto("Video Controls###video_controls", "Statistics###statistics"); // unsaved tweak
            ctx->Yield(3);
            IM_CHECK(coLocated(ctx, "Video Controls###video_controls", "Statistics###statistics"));

            OfsAppTestAccess::dispatchDisplayScaleChanged(app, savedDpi);
            ctx->Yield(3);
            // Reverted to the saved baseline ⇒ the unsaved redock is gone.
            IM_CHECK(!coLocated(ctx, "Video Controls###video_controls", "Statistics###statistics"));

            selectDefaultLayout(ctx); // clean baseline for later tests
            ctx->Yield(3);
        };

    // A saved user layout's node sizes were captured at its save-time content scale; applyLayoutIni
    // rescales them by current/saved on load. The dockspace has a central node, so scaling every node's
    // SizeRef grows the peripheral panels (their SizeRef is honored) while the central video area absorbs
    // the slack — at 2x the right column widens and the video narrows.
    IM_REGISTER_TEST(e, "layouts", "saved_layout_rescales_for_dpi_on_apply")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        selectDefaultLayout(ctx);

        auto columnWidth = [&]() -> float {
            ImGuiDockNode *c = columnNode(ctx, "Statistics###statistics");
            return c != nullptr ? c->Size.x : -1.0f;
        };
        auto centralWidth = [&]() -> float {
            ImGuiDockNode *c = dockedNode(ctx, "Video Player###video_player");
            return c != nullptr ? c->Size.x : -1.0f;
        };

        // Capture the scale-1 arrangement, as a saved layout's ini holds it.
        size_t n = 0;
        const char *iniRaw = ImGui::SaveIniSettingsToMemory(&n);
        const std::string ini(iniRaw, n);

        ofs::ui::applyLayoutIni(ini, 1.0f); // verbatim baseline
        ctx->Yield(3);
        const float col1 = columnWidth();
        const float central1 = centralWidth();
        IM_CHECK_GT(col1, 0.0f);
        IM_CHECK_GT(central1, 0.0f);

        ofs::ui::applyLayoutIni(ini, 2.0f); // apply the scale-1 layout as if at 2x
        ctx->Yield(3);
        IM_CHECK_GT(columnWidth(), col1 * 1.4f); // peripheral column grew
        IM_CHECK_LT(centralWidth(), central1);   // central video absorbed the slack

        // Rebuild Default directly to drop the rescaled nodes. selectDefaultLayout() can't here: the active
        // layout was never switched off "Default", so its menu item is a no-op and would leak the 2x
        // arrangement into the next suite.
        ofs::ui::applyDefaultLayout();
        ctx->Yield(3);
    };
}
