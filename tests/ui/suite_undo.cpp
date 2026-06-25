#include "Core/Events.h"
#include "Core/ScriptAxisAction.h"
#include "Core/StandardAxis.h"
#include "Core/VectorSet.h"
#include "helpers/TestState.h"
#include <imgui_te_engine.h>

void RegisterUndoTests(ImGuiTestEngine *e) {
    IM_REGISTER_TEST(e, "undo", "undo_reverts_axis_mutation")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &proj = *getTestState().project;
        auto &eq = *getTestState().eventQueue;

        IM_CHECK_EQ(proj.axes[0].actions.size(), static_cast<size_t>(0));

        ofs::VectorSet<ofs::ScriptAxisAction> actions;
        actions.insert({1.0, 50});
        eq.push(ofs::CommitAxisActionsEvent{.axis = ofs::StandardAxis::L0, .actions = actions});
        ctx->Yield();

        IM_CHECK_EQ(proj.axes[0].actions.size(), static_cast<size_t>(1));

        eq.push(ofs::UndoEvent{});
        ctx->Yield();

        IM_CHECK_EQ(proj.axes[0].actions.size(), static_cast<size_t>(0));
    };

    IM_REGISTER_TEST(e, "undo", "redo_reapplies_mutation")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &proj = *getTestState().project;
        auto &eq = *getTestState().eventQueue;

        ofs::VectorSet<ofs::ScriptAxisAction> actions;
        actions.insert({2.0, 75});
        eq.push(ofs::CommitAxisActionsEvent{.axis = ofs::StandardAxis::L0, .actions = actions});
        ctx->Yield();

        eq.push(ofs::UndoEvent{});
        ctx->Yield();
        IM_CHECK_EQ(proj.axes[0].actions.size(), static_cast<size_t>(0));

        eq.push(ofs::RedoEvent{});
        ctx->Yield();
        IM_CHECK_EQ(proj.axes[0].actions.size(), static_cast<size_t>(1));
        IM_CHECK_EQ(proj.axes[0].actions[0].pos, 75);
    };
}
