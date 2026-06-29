#include "Services/EditIntentRouter.h"

#include "Core/EventQueue.h"
#include "Core/Events.h"
#include "Core/IntentEvents.h"
#include "Core/OverlaySettings.h"
#include "Core/ProjectLifecycleEvents.h"
#include "Core/ScriptProject.h"
#include "Services/EditModeRegistry.h"
#include "Services/ModeLifecycle.h"
#include "Services/PluginApi.h"
#include "Services/PluginEvents.h"
#include "Util/Log.h"

#include <algorithm>
#include <vector>

namespace ofs {

namespace {
// EditIntent ↔ OfsEditIntent. The ABI struct mirrors the host EditIntent field-for-field, so both
// directions are a plain copy (plus the kind/axis casts and an axis bounds-clamp on the way in).
OfsEditIntent toAbi(const EditIntent &i) {
    return OfsEditIntent{
        .kind = static_cast<int>(i.kind),
        .axis = static_cast<int>(i.axis),
        .time = i.time,
        .fromTime = i.fromTime,
        .pos = i.pos,
        .direction = static_cast<int>(i.direction),
        .reps = std::max(1, i.reps),
        .exact = i.exact ? 1 : 0,
        .seekAfter = i.seekAfter ? 1 : 0,
    };
}

EditIntent fromAbi(const OfsEditIntent &a) {
    EditIntent i{};
    i.kind = static_cast<EditIntentKind>(a.kind);
    // A mode could emit a bad axis; clamp to a valid role so resolveNative never indexes out of range.
    i.axis = (a.axis >= 0 && a.axis < static_cast<int>(StandardAxis::Count)) ? static_cast<StandardAxis>(a.axis)
                                                                             : StandardAxis::L0;
    i.time = a.time;
    i.fromTime = a.fromTime;
    i.pos = a.pos;
    i.direction = static_cast<StepDirection>(a.direction);
    i.reps = std::max(1, a.reps);
    i.exact = a.exact != 0;
    i.seekAfter = a.seekAfter != 0;
    return i;
}

// True when `intent` edits an action identified by time — MovePoint's `fromTime`, RemovePoint's `time` —
// and that action exists on `axis`. Every other kind either creates an action or acts on the selection,
// so it names no pre-existing action and this is vacuously true. Used to drop a per-axis fan-out follower
// whose actions don't line up with the lead's source coordinate (expressed in the lead axis's own time).
bool sourceActionExists(const EditIntent &intent, const AxisState &axis) {
    switch (intent.kind) {
    case EditIntentKind::MovePoint:
        return axis.actions.contains(ScriptAxisAction{intent.fromTime, 0});
    case EditIntentKind::RemovePoint:
        return axis.actions.contains(ScriptAxisAction{intent.time, 0});
    default:
        return true;
    }
}

// The host-owned sink an edit mode's onEditIntent emits Replace intents into; collected into a vector
// the router resolves after the callback returns. The plugin copies in during the call.
void collectEmitted(void *sink, const OfsEditIntent *out) {
    if (out)
        static_cast<std::vector<OfsEditIntent> *>(sink)->push_back(*out);
}
} // namespace

EditIntentRouter::EditIntentRouter(ScriptProject &project, EventQueue &eq, EditModeRegistry &registry)
    : project(project), eq(eq), registry(registry) {
    eq.on<EditRequestEvent>([this](const EditRequestEvent &e) { onEditRequest(e); });
    eq.on<SetActiveEditModeEvent>([this](const SetActiveEditModeEvent &e) { onSetActiveEditMode(e); });
    eq.on<RegisterEditModeEvent>([this](const RegisterEditModeEvent &e) { onRegisterEditMode(e); });
    eq.on<UnregisterEditModesEvent>([this](const UnregisterEditModesEvent &e) { onUnregisterEditModes(e); });
    eq.on<LoadProjectEvent>([this](const LoadProjectEvent &e) { onProjectLoaded(e); });
}

void EditIntentRouter::onEditRequest(const EditRequestEvent &event) {
    // Arm the snapshot latch at a gesture boundary. Begin opens a coalesced gesture; OneShot is a
    // self-contained edit that snapshots on its own. A Continue frame leaves the latch as the Begin
    // frame's first mutation left it (cleared) so the rest of the gesture coalesces.
    if (event.gesture == GesturePhase::Begin || event.gesture == GesturePhase::OneShot)
        snapshotPending_ = true;

    // A native mode (or an unknown active id — a weak reference whose plugin has gone) resolves the
    // intent directly into its mutation event. A plugin mode gets a say first.
    const EditModeEntry *mode = registry.find(project.activeEditMode);
    if (mode == nullptr || mode->onEditIntent == nullptr) {
        resolveNative(event.intent);
        return;
    }

    // A selection nudge that moves a single action is presented to the mode as a MovePoint, so a mode
    // implementing only MovePoint governs single-action keyboard moves the same as mouse drags. A
    // multi-action nudge crosses as MoveSelection. Either way Pass falls back to the original nudge, so
    // native resolution stays byte-identical (the synthesized MovePoint is only the mode's lens).
    if (event.intent.kind == EditIntentKind::MoveSelection && isSingleActionNudge(event.intent)) {
        consultMode(*mode, synthesizeSingleMove(event.intent), event.intent);
        return;
    }
    consultMode(*mode, event.intent, event.intent);
}

void EditIntentRouter::consultMode(const EditModeEntry &mode, const EditIntent &consult,
                                   const EditIntent &nativeFallback) {
    const OfsEditIntent in = toAbi(consult);
    std::vector<OfsEditIntent> emitted;
    const int disposition = mode.onEditIntent(mode.userData, &in, &collectEmitted, &emitted);
    switch (disposition) {
    case OfsEditDrop:
        break; // discard — emit nothing, take no snapshot for a no-op
    case OfsEditReplace:
        // Resolve each replacement natively; the snapshot latch makes the first emitted mutation open
        // the undo step and the rest coalesce, however many a single Replace produced. Each resolves with
        // fanToGroup=true, so a lead-axis mutation still projects across the group below the seam.
        for (const OfsEditIntent &out : emitted)
            resolveNative(fromAbi(out));
        break;
    case OfsEditReplacePerAxis: {
        // Convert the lead's emitted replacements to host intents at the seam, so the per-axis helper (and
        // the header) deals only in host types — ABI marshaling stays here.
        std::vector<EditIntent> leadEmitted;
        leadEmitted.reserve(emitted.size());
        for (const OfsEditIntent &out : emitted)
            leadEmitted.push_back(fromAbi(out));
        resolvePerAxis(mode, consult, leadEmitted);
        break;
    }
    case OfsEditPass:
    default:
        // Pass (and any unknown disposition, defensively) applies the fallback intent unchanged. For a
        // single-action nudge the fallback is the original MoveSelection, not the MovePoint the mode saw.
        resolveNative(nativeFallback);
        break;
    }
}

bool EditIntentRouter::isSingleActionNudge(const EditIntent &intent) const {
    const auto lead = intent.axis;
    if (lead >= StandardAxis::Count)
        return false;
    const AxisState &a = project.axes[static_cast<size_t>(lead)];
    const size_t selN = a.selection.size();
    if (selN > 1)
        return false; // a real multi-selection — the mode sees the aggregate
    if (selN == 1)
        return true; // exactly one selected action
    // No selection: ProjectManager's nudge falls back to the action nearest the playhead — single iff one exists.
    return closestActionByTime(a.actions, project.playback.cursorPos) != nullptr;
}

EditIntent EditIntentRouter::synthesizeSingleMove(const EditIntent &intent) const {
    const AxisState &a = project.axes[static_cast<size_t>(intent.axis)];
    // isSingleActionNudge guarantees one of these is non-null.
    const ScriptAxisAction *src =
        a.selection.size() == 1 ? &(*a.selection.begin()) : closestActionByTime(a.actions, project.playback.cursorPos);

    EditIntent mp{};
    mp.kind = EditIntentKind::MovePoint;
    mp.axis = intent.axis;
    mp.fromTime = src->at;
    if (intent.direction != StepDirection::None) { // time nudge
        const double dt = static_cast<double>(static_cast<int>(intent.direction) * std::max(1, intent.reps)) *
                          stepTime(project.overlay);
        mp.time = std::max(0.0, src->at + dt);
        mp.pos = src->pos;
    } else { // position nudge
        mp.time = src->at;
        mp.pos = std::clamp(src->pos + intent.pos, 0, 100);
    }
    return mp;
}

void EditIntentRouter::resolvePerAxis(const EditModeEntry &mode, const EditIntent &leadIntent,
                                      const std::vector<EditIntent> &leadEmitted) {
    // Fan-out ABOVE the seam: the mode is consulted once per editable axis and each axis's result applies
    // verbatim to that axis alone — no mechanical projection. The lead axis is the gesture's axis; the
    // editable set mirrors ProjectManager's editTargets (the active group when the lead is the active axis,
    // else just the lead). Every resolved mutation carries fanToGroup=false so ProjectManager does NOT
    // re-project it across the group (which would double-apply); the router has already done the fan-out.
    const StandardAxis lead = leadIntent.axis;
    AxisRoles editable;
    if (lead == project.state.activeAxis) {
        editable = project.effectiveEditSet();
    } else if (lead < StandardAxis::Count) {
        editable.set(static_cast<size_t>(lead)); // a non-lead-targeted request stays single-axis
    }

    // Lead: apply its already-emitted intents verbatim. The snapshot latch opens the undo step on the
    // first mutation here and every later per-axis mutation coalesces into it (host-stamped below the seam).
    for (const EditIntent &out : leadEmitted)
        resolveNative(out, /*fanToGroup=*/false);

    // Followers: re-consult the mode once per remaining editable axis, the original request retargeted to
    // that axis, and honor the disposition it returns per axis — the same Pass/Drop/Replace branch
    // consultMode applies to the lead, so a follower that returns Pass gets its native fallback rather than
    // being silently dropped. ReplacePerAxis returned here degrades to a plain Replace (one level deep) —
    // the fan-out never recurses, so these re-consultations are leaves.
    const OfsEditIntent base = toAbi(leadIntent);
    for (size_t i = 0; i < kStandardAxisCount; ++i) {
        const auto role = static_cast<StandardAxis>(i);
        if (!editable.test(i) || role == lead || project.axes[i].isLocked)
            continue;
        // The lead's source coordinate (which action to move/remove) is in the lead axis's time; a follower
        // whose actions don't line up has no such action. Drop it rather than re-consulting the mode with a
        // coordinate that names nothing here — the mode can't detect the mismatch (it never sees the lead),
        // and resolving it would erase nothing then insert a phantom point. Mirrors native group projection,
        // which skips a member lacking the point (ProjectManager::onMoveAction / onRemoveActionAtTime).
        if (!sourceActionExists(leadIntent, project.axes[i]))
            continue;
        OfsEditIntent followerIn = base;
        followerIn.axis = static_cast<int>(role);
        std::vector<OfsEditIntent> followerEmitted;
        const int disposition = mode.onEditIntent(mode.userData, &followerIn, &collectEmitted, &followerEmitted);
        switch (disposition) {
        case OfsEditDrop:
            break; // this axis opts out — emit nothing
        case OfsEditReplace:
        case OfsEditReplacePerAxis: // degrades to Replace here (leaf re-consultation, no recursion)
            for (const OfsEditIntent &out : followerEmitted)
                resolveNative(fromAbi(out), /*fanToGroup=*/false);
            break;
        case OfsEditPass:
        default:
            // Pass (and any unknown disposition, defensively) resolves this axis's retargeted intent
            // natively — the per-axis analogue of consultMode's native fallback.
            resolveNative(fromAbi(followerIn), /*fanToGroup=*/false);
            break;
        }
    }
}

void EditIntentRouter::resolveNative(const EditIntent &intent, bool fanToGroup) {
    switch (intent.kind) {
    case EditIntentKind::AddPoint:
        eq.push(AddActionAtTimeEvent{.axis = intent.axis,
                                     .time = intent.time,
                                     .pos = intent.pos,
                                     .snapshot = consumeSnapshot(),
                                     .fanToGroup = fanToGroup});
        break;
    case EditIntentKind::AddPointAtPlayhead: {
        // Sugar decomposition: the playhead add resolves to a timed add at the active axis/cursor,
        // emitting the mutation directly (never a second request, so a mode sees the gesture once).
        const auto role = project.state.activeAxis;
        if (role >= StandardAxis::Count)
            break;
        eq.push(AddActionAtTimeEvent{.axis = role,
                                     .time = project.playback.cursorPos,
                                     .pos = intent.pos,
                                     .snapshot = consumeSnapshot(),
                                     .fanToGroup = fanToGroup});
        break;
    }
    case EditIntentKind::MovePoint:
        eq.push(MoveActionEvent{.axis = intent.axis,
                                .fromAt = intent.fromTime,
                                .toAt = intent.time,
                                .toPos = intent.pos,
                                .snapshot = consumeSnapshot(),
                                .fanToGroup = fanToGroup});
        break;
    case EditIntentKind::RemovePoint:
        eq.push(RemoveActionAtTimeEvent{
            .axis = intent.axis, .time = intent.time, .fanToGroup = fanToGroup, .snapshot = consumeSnapshot()});
        break;
    case EditIntentKind::RemoveSelected:
        eq.push(
            RemoveSelectedActionsEvent{.axis = intent.axis, .fanToGroup = fanToGroup, .snapshot = consumeSnapshot()});
        break;
    case EditIntentKind::MoveSelection:
        // Merged nudge: a time nudge (direction != 0) or a position nudge (the pos delta). Each resolves to
        // the existing selection-move event, so multi-action and native single-action behavior are unchanged.
        if (intent.direction != StepDirection::None)
            eq.push(MoveSelectionTimeEvent{.axis = intent.axis,
                                           .direction = intent.direction,
                                           .seekAfter = intent.seekAfter,
                                           .reps = intent.reps,
                                           .snapshot = consumeSnapshot(),
                                           .fanToGroup = fanToGroup});
        else
            eq.push(MoveSelectionPositionEvent{
                .axis = intent.axis, .delta = intent.pos, .snapshot = consumeSnapshot(), .fanToGroup = fanToGroup});
        break;
    case EditIntentKind::Paste:
        eq.push(PasteActionsEvent{.pasteTime = intent.time, .exact = intent.exact, .snapshot = consumeSnapshot()});
        break;
    }
}

void EditIntentRouter::onSetActiveEditMode(const SetActiveEditModeEvent &event) {
    // The footer selector is the only writer of activeEditMode/storedEditMode (see ModeLifecycle.h).
    mode_lifecycle::setActive(registry, project.activeEditMode, project.storedEditMode, event.id);
}

void EditIntentRouter::onRegisterEditMode(const RegisterEditModeEvent &event) {
    // Sole owner of the registry's plugin entries: publish the mode so the footer lists it and a later
    // user selection can activate it. Registration never activates (no plugin-callable setter), except to
    // restore the authored selection a project-load fallback left on native — see onEntryRegistered.
    registry.add(event.entry);
    mode_lifecycle::onEntryRegistered(registry, project.activeEditMode, project.storedEditMode, event.entry.id);
}

void EditIntentRouter::onUnregisterEditModes(const UnregisterEditModesEvent &event) {
    mode_lifecycle::unregisterPlugin(registry, project.activeEditMode, event.pluginName, kNativeEditModeId);
}

bool EditIntentRouter::consumeSnapshot() {
    const bool snap = snapshotPending_;
    snapshotPending_ = false;
    return snap;
}

void EditIntentRouter::onProjectLoaded(const LoadProjectEvent &) {
    mode_lifecycle::onProjectLoaded(registry, project.activeEditMode, project.storedEditMode, kNativeEditModeId,
                                    "Edit mode");
}

} // namespace ofs
