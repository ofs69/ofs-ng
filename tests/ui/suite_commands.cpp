#include "Core/Events.h"
#include "Core/ScriptAxisAction.h"
#include "Core/StandardAxis.h"
#include "Core/VectorSet.h"
#include "Services/CommandRegistry.h"
#include "Video/VideoPlayer.h"
#include "helpers/TestState.h"
#include <cmath>
#include <imgui.h>
#include <imgui_te_context.h>
#include <imgui_te_engine.h>
#include <initializer_list>
#include <vector>

// Behavioral tests for the static command table (src/App/OfsAppCommands.cpp). Each test invokes a
// command through the live CommandRegistry (the real dispatch path the palette / keyboard use) and
// asserts the command's observable effect on project / player state — not merely that it ran. The
// holdable commands are also driven through tick() to prove the hold-repeat path nudges the same way.

using namespace ofs;

namespace {

constexpr size_t kL0 = static_cast<size_t>(StandardAxis::L0);

void run(const char *id) {
    getTestState().commandRegistry->run(id);
}

// Tick a holdable command once, well past the initial delay so holdRepeats() yields a real burst.
void tick(const char *id) {
    const Command *c = getTestState().commandRegistry->find(id);
    IM_CHECK(c != nullptr && c->holdable());
    c->tick(*getTestState().eventQueue, HoldTickInfo{.dt = 0.1f, .elapsed = 5.0f, .first = true});
}

// Replace L0's actions and make L0 the active axis. Returns once applied.
void seedL0(ImGuiTestContext *ctx, std::initializer_list<ScriptAxisAction> acts) {
    VectorSet<ScriptAxisAction> v;
    for (const auto &a : acts)
        v.insert(a);
    auto &eq = *getTestState().eventQueue;
    eq.push(AxisSelectedEvent{StandardAxis::L0});
    eq.push(CommitAxisActionsEvent{.axis = StandardAxis::L0, .actions = v});
    ctx->Yield(2);
}

void selectAllL0(ImGuiTestContext *ctx) {
    getTestState().eventQueue->push(SelectRequestEvent{.gesture = SelectGesture::All, .axis = StandardAxis::L0});
    ctx->Yield(2);
}

bool anyPopupOpen() {
    return ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
}

} // namespace

void RegisterCommandsTests(ImGuiTestEngine *e) {
    // ── Player transport ──────────────────────────────────────────────────────────────────────────
    IM_REGISTER_TEST(e, "commands", "player_goto_start_and_end")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &proj = *getTestState().project;
        auto *player = getTestState().videoPlayer;
        const double dur = player->getDuration();
        IM_CHECK_GT(dur, 1.0);

        getTestState().eventQueue->push(SeekEvent{dur * 0.5});
        ctx->Yield(2);
        run("player.goto-start");
        ctx->Yield(2);
        IM_CHECK_EQ(proj.playback.cursorPos, 0.0);

        run("player.goto-end");
        ctx->Yield(2);
        IM_CHECK_LT(std::abs(proj.playback.cursorPos - dur), 0.01);
    };

    IM_REGISTER_TEST(e, "commands", "player_play_pause_toggles")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto *player = getTestState().videoPlayer;
        IM_CHECK(player->isPaused());
        run("player.play-pause");
        ctx->Yield(2);
        IM_CHECK_EQ(player->isPaused(), false);
        run("player.play-pause");
        ctx->Yield(2);
        IM_CHECK_EQ(player->isPaused(), true);
    };

    IM_REGISTER_TEST(e, "commands", "player_speed_up_down")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto *player = getTestState().videoPlayer;
        IM_CHECK_LT(std::abs(player->getPlaybackSpeed() - 1.0f), 0.01f); // reset on media load
        run("player.speed-up");
        ctx->Yield(2);
        IM_CHECK_LT(std::abs(player->getPlaybackSpeed() - 1.1f), 0.01f);
        // Each step reads the player's live speed, so the event must drain between presses.
        run("player.speed-down");
        ctx->Yield(2);
        run("player.speed-down");
        ctx->Yield(2);
        IM_CHECK_LT(std::abs(player->getPlaybackSpeed() - 0.9f), 0.01f);

        // Holding the key (the tick path) steps speed too.
        tick("player.speed-up");
        ctx->Yield(2);
        IM_CHECK_GT(player->getPlaybackSpeed(), 0.9f);
    };

    IM_REGISTER_TEST(e, "commands", "player_volume_up_down_and_mute")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &eq = *getTestState().eventQueue;
        auto *player = getTestState().videoPlayer;

        eq.push(VolumeChangedEvent{0.5f});
        ctx->Yield(2);
        IM_CHECK_LT(std::abs(player->getVolume() - 0.5f), 0.01f);

        run("player.volume-up");
        ctx->Yield(2);
        IM_CHECK_LT(std::abs(player->getVolume() - 0.6f), 0.01f);
        // Each step reads the player's live volume, so the event must drain between presses.
        run("player.volume-down");
        ctx->Yield(2);
        run("player.volume-down");
        ctx->Yield(2);
        IM_CHECK_LT(std::abs(player->getVolume() - 0.4f), 0.01f);

        // Mute stashes the level and drops to 0; muting again restores it.
        run("player.toggle-mute");
        ctx->Yield(2);
        IM_CHECK_LT(player->getVolume(), 0.01f);
        run("player.toggle-mute");
        ctx->Yield(2);
        IM_CHECK_LT(std::abs(player->getVolume() - 0.4f), 0.01f);
    };

    // ── Axis ──────────────────────────────────────────────────────────────────────────────────────
    IM_REGISTER_TEST(e, "commands", "axis_add_scratch")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &proj = *getTestState().project;
        const size_t s0 = static_cast<size_t>(StandardAxis::S0);
        IM_CHECK(!proj.axes[s0].showInStrip); // fixture ships with no scratch axes

        const Command *c = getTestState().commandRegistry->find("axis.add-scratch");
        IM_CHECK(c != nullptr && c->enabled()); // a free scratch slot ⇒ enabled
        run("axis.add-scratch");
        ctx->Yield(2);
        IM_CHECK(proj.axes[s0].showInStrip);
    };

    // ── Navigation ──────────────────────────────────────────────────────────────────────────────
    IM_REGISTER_TEST(e, "commands", "nav_cycle_axis_is_reversible")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &proj = *getTestState().project;
        auto &eq = *getTestState().eventQueue;
        // Cycling walks the axes shown in the strip; add a scratch axis so there are at least two.
        eq.push(AddScratchAxisEvent{});
        ctx->Yield(2);
        std::vector<StandardAxis> panel;
        for (size_t i = 0; i < kStandardAxisCount; ++i)
            if (proj.axes[i].showInStrip)
                panel.push_back(static_cast<StandardAxis>(i));
        IM_CHECK_GE(panel.size(), static_cast<size_t>(2));

        eq.push(AxisSelectedEvent{panel[0]});
        ctx->Yield(2);
        run("navigation.cycle-axis-forward");
        ctx->Yield(2);
        IM_CHECK(proj.state.activeAxis == panel[1]); // forward → next strip axis

        run("navigation.cycle-axis-backward");
        ctx->Yield(2);
        IM_CHECK(proj.state.activeAxis == panel[0]); // backward returns
    };

    IM_REGISTER_TEST(e, "commands", "nav_next_prev_action")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &proj = *getTestState().project;
        auto &eq = *getTestState().eventQueue;
        seedL0(ctx, {{1.0, 20}, {2.0, 40}, {3.0, 60}});

        eq.push(SeekEvent{1.4});
        ctx->Yield(2);
        run("navigation.next-action");
        ctx->Yield(2);
        IM_CHECK_LT(std::abs(proj.playback.cursorPos - 2.0), 0.01); // landed on the next action

        run("navigation.prev-action");
        ctx->Yield(2);
        IM_CHECK_LT(std::abs(proj.playback.cursorPos - 1.0), 0.01); // and back to the previous one
    };

    IM_REGISTER_TEST(e, "commands", "nav_next_prev_action_multi")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &proj = *getTestState().project;
        auto &eq = *getTestState().eventQueue;
        seedL0(ctx, {{1.0, 20}, {2.0, 40}, {3.0, 60}});

        // The "-multi" variants step to the nearest action across all axes (here only L0 has any).
        eq.push(SeekEvent{1.4});
        ctx->Yield(2);
        run("navigation.next-action-multi");
        ctx->Yield(2);
        IM_CHECK_LT(std::abs(proj.playback.cursorPos - 2.0), 0.01);

        run("navigation.prev-action-multi");
        ctx->Yield(2);
        IM_CHECK_LT(std::abs(proj.playback.cursorPos - 1.0), 0.01);
    };

    IM_REGISTER_TEST(e, "commands", "nav_next_prev_step")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &proj = *getTestState().project;
        auto &eq = *getTestState().eventQueue;

        eq.push(SeekEvent{5.0});
        ctx->Yield(2);
        run("navigation.next-step");
        ctx->Yield(2);
        IM_CHECK_GT(proj.playback.cursorPos, 5.0); // a grid step forward

        eq.push(SeekEvent{5.0});
        ctx->Yield(2);
        run("navigation.prev-step");
        ctx->Yield(2);
        IM_CHECK_LT(proj.playback.cursorPos, 5.0); // a grid step back
    };

    // ── Edit: add point ─────────────────────────────────────────────────────────────────────────
    IM_REGISTER_TEST(e, "commands", "edit_add_action_at_playhead")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &proj = *getTestState().project;
        auto &eq = *getTestState().eventQueue;
        seedL0(ctx, {}); // empty, L0 active
        IM_CHECK_EQ(proj.axes[kL0].actions.size(), static_cast<size_t>(0));

        eq.push(SeekEvent{4.0});
        ctx->Yield(2);
        run("edit.add-action-50");
        ctx->Yield(3);
        IM_CHECK_EQ(proj.axes[kL0].actions.size(), static_cast<size_t>(1));
        IM_CHECK(proj.axes[kL0].actions.contains(ScriptAxisAction{4.0, 0}));
        IM_CHECK_EQ(proj.axes[kL0].actions[0].pos, 50); // the keypad-5 command writes pos 50

        eq.push(SeekEvent{6.0});
        ctx->Yield(2);
        run("edit.add-action-100");
        ctx->Yield(3);
        IM_CHECK_EQ(proj.axes[kL0].actions.size(), static_cast<size_t>(2));
        IM_CHECK(proj.axes[kL0].actions.contains(ScriptAxisAction{6.0, 0}));
    };

    // ── Edit: selection ─────────────────────────────────────────────────────────────────────────
    IM_REGISTER_TEST(e, "commands", "edit_select_all_and_deselect")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &proj = *getTestState().project;
        seedL0(ctx, {{1.0, 10}, {2.0, 20}, {3.0, 30}});

        run("edit.select-all");
        ctx->Yield(2);
        IM_CHECK_EQ(proj.axes[kL0].selection.size(), proj.axes[kL0].actions.size());

        run("edit.deselect-all");
        ctx->Yield(2);
        IM_CHECK(proj.axes[kL0].selection.empty());
    };

    IM_REGISTER_TEST(e, "commands", "edit_select_left_and_right_of_playhead")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &proj = *getTestState().project;
        auto &eq = *getTestState().eventQueue;
        seedL0(ctx, {{1.0, 10}, {2.0, 20}, {3.0, 30}, {4.0, 40}});

        eq.push(SeekEvent{2.5}); // splits the four points 2 | 2
        ctx->Yield(2);

        run("edit.select-all-left");
        ctx->Yield(2);
        IM_CHECK_EQ(proj.axes[kL0].selection.size(), static_cast<size_t>(2));
        for (const auto &a : proj.axes[kL0].selection)
            IM_CHECK_LE(a.at, 2.5);

        run("edit.select-all-right");
        ctx->Yield(2);
        IM_CHECK_EQ(proj.axes[kL0].selection.size(), static_cast<size_t>(2));
        for (const auto &a : proj.axes[kL0].selection)
            IM_CHECK_GE(a.at, 2.5);
    };

    // ── Edit: clipboard ─────────────────────────────────────────────────────────────────────────
    IM_REGISTER_TEST(e, "commands", "edit_copy_then_paste_duplicates")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &proj = *getTestState().project;
        auto &eq = *getTestState().eventQueue;
        seedL0(ctx, {{1.0, 10}, {2.0, 20}});

        selectAllL0(ctx);
        run("edit.copy");
        ctx->Yield(2);

        eq.push(SeekEvent{10.0}); // paste anchor well clear of the originals
        ctx->Yield(2);
        run("edit.paste");
        ctx->Yield(3);
        IM_CHECK_EQ(proj.axes[kL0].actions.size(), static_cast<size_t>(4)); // two pasted alongside the two originals

        // Paste-exact drops the clipboard at its original times — pasting again over the now-occupied
        // 1 s / 2 s slots is a no-op, leaving the count unchanged.
        eq.push(SeekEvent{1.0});
        ctx->Yield(2);
        run("edit.paste-exact");
        ctx->Yield(3);
        IM_CHECK_EQ(proj.axes[kL0].actions.size(), static_cast<size_t>(4));
    };

    IM_REGISTER_TEST(e, "commands", "edit_cut_removes_selection")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &proj = *getTestState().project;
        seedL0(ctx, {{1.0, 10}, {2.0, 20}, {3.0, 30}});

        selectAllL0(ctx);
        run("edit.cut");
        ctx->Yield(3);
        IM_CHECK_EQ(proj.axes[kL0].actions.size(), static_cast<size_t>(0));
    };

    IM_REGISTER_TEST(e, "commands", "edit_remove_action_drops_selection")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &proj = *getTestState().project;
        IM_CHECK_EQ(proj.procSelRegionId, -1); // no region selected ⇒ falls through to the axis selection
        seedL0(ctx, {{1.0, 10}, {2.0, 20}, {3.0, 30}});

        // Select just the middle point so the delete is observable as a count of one.
        getTestState().eventQueue->push(SelectRequestEvent{
            .gesture = SelectGesture::Point, .axis = StandardAxis::L0, .startTime = 2.0, .endTime = 2.0});
        ctx->Yield(2);
        IM_CHECK_EQ(proj.axes[kL0].selection.size(), static_cast<size_t>(1));

        run("edit.remove-action");
        ctx->Yield(3);
        IM_CHECK_EQ(proj.axes[kL0].actions.size(), static_cast<size_t>(2));
        IM_CHECK(!proj.axes[kL0].actions.contains(ScriptAxisAction{2.0, 0}));
    };

    // ── Edit: undo / redo (via the holdable commands) ───────────────────────────────────────────
    IM_REGISTER_TEST(e, "commands", "edit_undo_redo")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &proj = *getTestState().project;
        auto &eq = *getTestState().eventQueue;
        seedL0(ctx, {});

        eq.push(SeekEvent{3.0});
        ctx->Yield(2);
        run("edit.add-action-30");
        ctx->Yield(3);
        IM_CHECK_EQ(proj.axes[kL0].actions.size(), static_cast<size_t>(1));

        run("edit.undo");
        ctx->Yield(2);
        IM_CHECK_EQ(proj.axes[kL0].actions.size(), static_cast<size_t>(0));

        run("edit.redo");
        ctx->Yield(2);
        IM_CHECK_EQ(proj.axes[kL0].actions.size(), static_cast<size_t>(1));
    };

    // ── Edit: move selection (position + time, run() and tick()) ────────────────────────────────
    IM_REGISTER_TEST(e, "commands", "edit_move_actions_up_down")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &proj = *getTestState().project;
        seedL0(ctx, {{1.0, 50}});
        selectAllL0(ctx);

        run("edit.move-actions-up");
        ctx->Yield(2);
        IM_CHECK_EQ(proj.axes[kL0].actions[0].pos, 51); // one step up

        run("edit.move-actions-down");
        run("edit.move-actions-down");
        ctx->Yield(2);
        IM_CHECK_EQ(proj.axes[kL0].actions[0].pos, 49); // two steps down

        // The hold path accumulates: one tick past the initial delay moves more than a single step.
        const int before = proj.axes[kL0].actions[0].pos;
        tick("edit.move-actions-up");
        ctx->Yield(2);
        IM_CHECK_GT(proj.axes[kL0].actions[0].pos, before + 1);
    };

    IM_REGISTER_TEST(e, "commands", "edit_move_actions_left_right")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &proj = *getTestState().project;
        seedL0(ctx, {{5.0, 50}});
        selectAllL0(ctx);

        const double before = proj.axes[kL0].actions[0].at;
        run("edit.move-actions-right");
        ctx->Yield(2);
        IM_CHECK_GT(proj.axes[kL0].actions[0].at, before); // shifted later in time

        const double afterRight = proj.axes[kL0].actions[0].at;
        run("edit.move-actions-left");
        ctx->Yield(2);
        IM_CHECK_LT(proj.axes[kL0].actions[0].at, afterRight); // shifted earlier
    };

    IM_REGISTER_TEST(e, "commands", "edit_move_actions_snapped_seeks")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &proj = *getTestState().project;
        auto &eq = *getTestState().eventQueue;
        seedL0(ctx, {{5.0, 50}});
        selectAllL0(ctx);
        eq.push(SeekEvent{5.0});
        ctx->Yield(2);

        // The "snapped" variants seek the playhead to follow the moved action.
        run("edit.move-actions-right-snapped");
        ctx->Yield(2);
        IM_CHECK_LT(std::abs(proj.playback.cursorPos - proj.axes[kL0].actions[0].at), 0.01);
    };

    IM_REGISTER_TEST(e, "commands", "edit_move_action_to_playhead")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &proj = *getTestState().project;
        auto &eq = *getTestState().eventQueue;
        seedL0(ctx, {{2.0, 50}, {8.0, 60}});

        eq.push(SeekEvent{2.4}); // closest action is the one at 2.0
        ctx->Yield(2);
        run("edit.move-action-to-playhead");
        ctx->Yield(3);
        IM_CHECK(proj.axes[kL0].actions.contains(ScriptAxisAction{2.4, 0}));  // moved onto the playhead
        IM_CHECK(!proj.axes[kL0].actions.contains(ScriptAxisAction{2.0, 0})); // vacated its old time
        IM_CHECK(proj.axes[kL0].actions.contains(ScriptAxisAction{8.0, 0}));  // the far one is untouched
    };

    // ── Core: command palette ───────────────────────────────────────────────────────────────────
    IM_REGISTER_TEST(e, "commands", "command_palette_open")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        run("command-palette.open");
        ctx->Yield(3);
        IM_CHECK(anyPopupOpen());
        ctx->KeyPress(ImGuiKey_Escape); // leave nothing open for the next suite
        ctx->Yield(2);
    };
}
