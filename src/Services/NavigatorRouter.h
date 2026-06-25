#pragma once

namespace ofs {

class EventQueue;
class NavigatorRegistry;
struct ScriptProject;
struct StepRequestEvent;
struct SetActiveNavigatorEvent;
struct RegisterNavigatorEvent;
struct UnregisterNavigatorsEvent;
struct LoadProjectEvent;

// Sole subscriber to StepRequestEvent — the navigation interception seam. It resolves a Step request
// into an absolute target and pushes a SeekEvent that the rest of the app consumes unchanged. The
// active navigator (ScriptProject::activeNavigator) selects the resolver; the built-in
// `follow-overlay` resolver defers to the current overlay (frame interval / tempo grid). When a plugin
// navigator is active, the router marshals the Step across the C ABI and calls onStep: a returned Seek
// pushes SeekEvent, Nav.None pushes nothing. The effective id is a weak reference: when its plugin is
// absent (foreign file / uninstalled on load, or disabled/unloaded/reloaded/crashed at runtime) it
// falls back to follow-overlay, while the stored authored id (ScriptProject::storedNavigator) is
// preserved for a re-save.
class NavigatorRouter {
  public:
    NavigatorRouter(ScriptProject &project, EventQueue &eq, NavigatorRegistry &registry);

  private:
    void onStepRequest(const StepRequestEvent &event);
    // Resolve a Step natively and push the SeekEvent; an ActionAllAxes step also activates the axis it
    // lands on. Used for the follow-overlay navigator and a plugin navigator's Pass.
    void pushNativeStep(const StepRequestEvent &event);
    void onSetActiveNavigator(const SetActiveNavigatorEvent &event);
    void onRegisterNavigator(const RegisterNavigatorEvent &event);
    void onUnregisterNavigators(const UnregisterNavigatorsEvent &event);
    void onProjectLoaded(const LoadProjectEvent &event);

    ScriptProject &project;
    EventQueue &eq;
    NavigatorRegistry &registry;
};

} // namespace ofs
