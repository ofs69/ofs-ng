#pragma once
#include "Core/Events.h"
#include "Format/AppSettings.h"
#include "Services/EffectRegistry.h"
#include "Services/JobSystem.h"
#include "Services/ProjectManager.h"
#include "Services/SelectIntentRouter.h"
#include "Services/SelectionModeRegistry.h"
#include "helpers/TestProject.h"

namespace ofs::test {
// ProjectManager wired to a fresh project + event queue, queue frozen, job system started.
struct PmFixture {
    TestProject tp;
    AppSettings appSettings;
    JobSystem jobSystem;
    EffectRegistryState effectReg;
    ProjectManager pm{tp.project, tp.eq, appSettings, jobSystem, effectReg};
    // Sole SelectRequestEvent subscriber — selection tests push SelectRequestEvent and the router
    // resolves it natively (per-axis loop + additive) through project.setSelection.
    SelectionModeRegistry selReg;
    SelectIntentRouter selRouter{tp.project, tp.eq, selReg};

    // User-facing notifications (toasts) emitted during a test, captured in push order.
    std::vector<NotifyEvent> notifications;

    PmFixture() {
        appSettings.autoBackupEnabled = false; // pm holds a const ref; effective immediately
        tp.project.axes[0].showInStrip = true; // L0 shown in the panel (the default active axis)
        tp.eq.on<NotifyEvent>([this](const NotifyEvent &e) { notifications.push_back(e); });
        tp.eq.freeze();
        jobSystem.start();
    }

    ScriptProject &project() { return tp.project; }
    EventQueue &eq() { return tp.eq; }

    // Push an event and apply it on the spot.
    template <typename E> void send(E e) {
        tp.eq.push(std::move(e));
        tp.eq.drain();
    }

    // Replace L0's actions with a fixed three-point set used by selection/action tests.
    void seedL0_3() {
        VectorSet<ScriptAxisAction> a;
        a.insert({1.0, 10});
        a.insert({2.0, 20});
        a.insert({3.0, 30});
        send(CommitAxisActionsEvent{.axis = StandardAxis::L0, .actions = a});
    }
};
} // namespace ofs::test
