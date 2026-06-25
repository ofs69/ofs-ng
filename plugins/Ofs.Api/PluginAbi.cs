using System;
using System.Runtime.InteropServices;

namespace Ofs
{
    // ─────────────────────────────────────────────────────────────────────────
    // C ABI mirror — INTERNAL. Plugin authors never see these types; they are
    // reachable only from Ofs.PluginHost via [InternalsVisibleTo]. Layout must
    // match src/Services/PluginApi.h exactly.
    // ─────────────────────────────────────────────────────────────────────────

    internal static class ApiVersions
    {
        // Guards the native↔managed boundary only: the HostApi/PluginApi struct layouts
        // below must match src/Services/PluginApi.h (OFS_ABI_VERSION). It does NOT gate
        // plugin compatibility — a plugin binds against the host's Ofs.Api at runtime
        // (PluginLoadContext discards the plugin's own copy), so this constant is always
        // the host's value and never reflects what a plugin was built against. That check
        // lives in PluginBootstrapper, against Ofs.Api's assembly version.
        // Keep in step with OFS_ABI_VERSION and the major of Ofs.Api's AssemblyVersion.
        public const int AbiVersion = 1;
    }

    // ── Plugin node types ─────────────────────────────────────────────────────

    internal enum OfsSignalKind { Discrete = 0, Functional = 1 }

    // Pure-value context passed to every node evaluation callback.
    [StructLayout(LayoutKind.Sequential)]
    internal unsafe struct OfsEvalCtx
    {
        public double RegionStart;
        public double RegionEnd;
        public float* Params;   // length == ParamCount
        public int ParamCount;
        public int StateHandle; // ≥0 → index into the managed TState capture set; -1 = none
        public int* CancelFlag; // 0 = run, nonzero = eval superseded; null when there's no owning job
    }

    // Mirrors the C OfsCommandDef struct; must match its layout exactly.
    [StructLayout(LayoutKind.Sequential)]
    internal unsafe struct OfsCommandDef
    {
        public byte* Id;
        public byte* Group; // if null, host defaults to plugin name
        public byte* Title;
        public int InRebindList; // 0 = not listed in the rebind UI by default; 1 = offered for binding there
        public int InPalette;    // 0 = hidden from the palette; 1 = searchable in the palette
    }


    // Mirrors the C OfsNodeDef struct; must match its layout exactly.
    [StructLayout(LayoutKind.Sequential)]
    internal unsafe struct OfsNodeDef
    {
        public byte* Id;
        public byte* DisplayName;
        public byte** InputNames;   // length == InputCount; pin labels (may be null when InputCount == 0)
        public byte** OutputNames;  // length == OutputCount
        public IntPtr Fn;           // evaluation callback (OfsDiscreteNodeFn / OfsFunctionalNodeFn by Signal)
        public void* UserData;
        public IntPtr OnNodeUi;      // OfsNodeUiFn; null = no custom UI
        public int Signal;          // OfsSignalKind
        public int InputCount;      // 0..16
        public int OutputCount;     // 1..16
        public int HasState;        // 1 → TState-backed registration
        public byte* Group;         // add-node menu group header; null/"" → host defaults to plugin name
        public int Icon;            // NodeIcon value; 0 (Default) → host uses the arity-bucket icon
    }

    // ── Interaction-intent types (edit modes + navigators) ────────────────────
    // Mirror src/Services/PluginApi.h exactly.

    internal enum OfsEditIntentKind
    {
        AddPoint = 0,
        AddPointAtPlayhead,
        MovePoint,
        RemovePoint,
        RemoveSelected,
        MoveSelection,
        Paste,
    }

    // onStep return disposition — mirrors OfsNavResult in PluginApi.h.
    internal enum OfsNavResult { None = 0, Seek = 1, Pass = 2 }

    internal enum OfsEditDisposition { Pass = 0, Drop = 1, Replace = 2, ReplacePerAxis = 3 }

    // Flat tagged edit intent; mirrors OfsEditIntent in PluginApi.h field-for-field (no packing).
    [StructLayout(LayoutKind.Sequential)]
    internal struct OfsEditIntent
    {
        public int Kind;
        public int Axis;
        public double Time;
        public double FromTime;
        public int Pos;
        public int Direction; // MoveSelection time nudge: +1 fwd / -1 back; 0 = position nudge (Pos = delta)
        public int Reps;      // MoveSelection time nudge: held-repeat burst count. Contract: always ≥ 1 (host clamps 0/neg)
        public int Exact;     // Paste: 1 = paste at original times
        public int SeekAfter; // MoveSelection time nudge: 1 = seek to the moved selection afterward
    }

    // Dual-purpose: `step` carries the request (Direction/Reps/Granularity), `out` carries the result
    // (Time + optional axis activation). No tag field — direction across the seam is fixed by which
    // argument it is.
    [StructLayout(LayoutKind.Sequential)]
    internal struct OfsNavIntent
    {
        public int Direction;
        public int Reps;        // Step: held-repeat burst count. Contract: always ≥ 1 (host clamps 0/neg)
        public double Time;
        public int Granularity; // OfsNavGranularity (Step); mirrors StepGranularity in IntentEvents.h
        public int Axis;        // Seek: StandardAxis to activate, or StandardAxis.Count = leave unchanged
    }

    // Edit-mode registration def. The callbacks are [UnmanagedCallersOnly] trampoline pointers; UserData
    // is the slot index the trampolines key off. OnEnter/OnExit are IntPtr.Zero when the author gave none.
    [StructLayout(LayoutKind.Sequential)]
    internal unsafe struct OfsEditModeDef
    {
        public byte* Id;
        public byte* DisplayName;
        public IntPtr OnEditIntent; // int (void* ud, OfsEditIntent* in, OfsEmitEdit emit, void* sink)
        public IntPtr OnEnter;      // void (void* ud)
        public IntPtr OnExit;       // void (void* ud)
        public IntPtr OnUi;         // void (void* ud); null = no options UI
        public void* UserData;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal unsafe struct OfsNavigatorDef
    {
        public byte* Id;
        public byte* DisplayName;
        public IntPtr OnStep;  // int (void* ud, OfsNavIntent* step, OfsNavIntent* out)
        public IntPtr OnEnter; // void (void* ud); IntPtr.Zero = none
        public IntPtr OnExit;  // void (void* ud); IntPtr.Zero = none
        public IntPtr OnUi;    // void (void* ud); null = no options UI
        public void* UserData;
    }

    // Selection-authoring gesture — mirrors OfsSelectGesture in PluginApi.h / SelectGesture in IntentEvents.h.
    internal enum OfsSelectGesture { Box = 0, All = 1, Point = 2 }

    // onSelect return disposition — mirrors OfsSelectDisposition in PluginApi.h.
    internal enum OfsSelectDisposition { Pass = 0, Drop = 1, Replace = 2 }

    // A selection request handed to an active selection mode, once per editable axis. The replace/toggle
    // combine is host-owned below the seam and not exposed here. Mirrors OfsSelectRequest in PluginApi.h.
    [StructLayout(LayoutKind.Sequential)]
    internal struct OfsSelectRequest
    {
        public int Gesture;
        public int Axis;
        public double StartTime;
        public double EndTime;
        public int Pos;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal unsafe struct OfsSelectModeDef
    {
        public byte* Id;
        public byte* DisplayName;
        public IntPtr OnSelect; // int (void* ud, OfsSelectRequest* in, OfsEmitSelect emit, void* sink)
        public IntPtr OnEnter;  // void (void* ud); IntPtr.Zero = none
        public IntPtr OnExit;   // void (void* ud); IntPtr.Zero = none
        public IntPtr OnUi;     // void (void* ud); null = no options UI
        public void* UserData;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal unsafe struct HostApi
    {
        public int Version; // must equal ApiVersions.AbiVersion
        public void* Ctx;   // host-supplied context; pass as first arg to every function below
        // Player queries
        public delegate* unmanaged[Cdecl]<void*, double> GetTime;
        public delegate* unmanaged[Cdecl]<void*, double> GetDuration;
        public delegate* unmanaged[Cdecl]<void*, int> IsPlaying;
        public delegate* unmanaged[Cdecl]<void*, float> GetSpeed;
        public delegate* unmanaged[Cdecl]<void*, byte*, int, int> GetMediaPath; // returns required len (excl NUL)
        public delegate* unmanaged[Cdecl]<void*, float> GetVolume;
        public delegate* unmanaged[Cdecl]<void*, float, void> SetVolume;
        public delegate* unmanaged[Cdecl]<void*, double> GetFps;
        public delegate* unmanaged[Cdecl]<void*, int> GetVideoWidth;
        public delegate* unmanaged[Cdecl]<void*, int> GetVideoHeight;
        // Seconds one action move-step travels under the active overlay (frame interval or tempo beat).
        public delegate* unmanaged[Cdecl]<void*, double> GetMoveStepTime;
        // Axis enumeration — returns total count; copies present roles (as int) into rolesBuf when non-null
        public delegate* unmanaged[Cdecl]<void*, int*, int, int> GetAxisRoles;
        public delegate* unmanaged[Cdecl]<void*, int> GetActiveAxisRole;
        // Axis read/write
        public delegate* unmanaged[Cdecl]<void*, int, ScriptAction*, int, int> GetAxisActions;
        public delegate* unmanaged[Cdecl]<void*, int, int> GetAxisActionCount;
        public delegate* unmanaged[Cdecl]<void*, int, ScriptAction*, int, void> CommitAxisActions;
        // Selection read/write
        public delegate* unmanaged[Cdecl]<void*, int, ScriptAction*, int, int> GetAxisSelection;
        public delegate* unmanaged[Cdecl]<void*, int, int> GetAxisSelectionCount;
        public delegate* unmanaged[Cdecl]<void*, int, ScriptAction*, int, void> SetAxisSelection;
        public delegate* unmanaged[Cdecl]<void*, int, void> ClearAxisSelection;
        // Player commands
        public delegate* unmanaged[Cdecl]<void*, double, void> SeekTo;
        public delegate* unmanaged[Cdecl]<void*, int, void> SetPlaying;
        public delegate* unmanaged[Cdecl]<void*, float, void> SetSpeed;
        public delegate* unmanaged[Cdecl]<void*, OfsCommandDef*, int> RegisterCommand;
        // Immediate-mode UI widget functions
        public delegate* unmanaged[Cdecl]<void*, byte*, void> UiLabel;
        public delegate* unmanaged[Cdecl]<void*, byte*, int> UiButton;
        public delegate* unmanaged[Cdecl]<void*, byte*, int*, int> UiCheckbox;
        public delegate* unmanaged[Cdecl]<void*, byte*, float*, float, float, int, int> UiSliderFloat;
        public delegate* unmanaged[Cdecl]<void*, byte*, int*, int, int, int> UiSliderInt;
        public delegate* unmanaged[Cdecl]<void*, byte*, int*, byte*, int> UiCombo;
        public delegate* unmanaged[Cdecl]<void*, void> UiSeparator;
        public delegate* unmanaged[Cdecl]<void*, byte*, void> UiPushSection;
        public delegate* unmanaged[Cdecl]<void*, void> UiPopSection;
        public delegate* unmanaged[Cdecl]<void*, byte*, int*, int, int> UiRadioButton;
        public delegate* unmanaged[Cdecl]<void*, byte*, int*, int, int> UiInputInt;
        public delegate* unmanaged[Cdecl]<void*, byte*, float*, float, int, int> UiInputFloat;
        public delegate* unmanaged[Cdecl]<void*, byte*, int*, float, int, int, int> UiDragInt;
        public delegate* unmanaged[Cdecl]<void*, byte*, float*, float, float, float, int, int> UiDragFloat;
        public delegate* unmanaged[Cdecl]<void*, byte*, byte*, int, int, int, int> UiInputText;
        public delegate* unmanaged[Cdecl]<void*, byte*, void> UiPushRow;
        public delegate* unmanaged[Cdecl]<void*, void> UiPopRow;
        // Plugin node registration — main thread only, call from OnLoad
        public delegate* unmanaged[Cdecl]<void*, OfsNodeDef*, void> RegisterNode;
        // Discrete I/O accessors — worker-thread-safe; take opaque OfsDiscreteInput/Output* as void*
        public delegate* unmanaged[Cdecl]<void*, int> NodeInputCount;
        public delegate* unmanaged[Cdecl]<void*, int, double> NodeInputTime;
        public delegate* unmanaged[Cdecl]<void*, int, int> NodeInputPosition;
        public delegate* unmanaged[Cdecl]<void*, double, int, void> NodeAddAction;
        // Host diagnostics — any thread. level matches LogLevel: 0=Trace 1=Debug 2=Info 3=Warning 4=Error.
        public delegate* unmanaged[Cdecl]<void*, int, byte*, void> HostLog;
        // Throttled user-facing error toast for a swallowed plugin fault — any thread. arg is the
        // faulting entry-point name; host names the plugin and coalesces.
        public delegate* unmanaged[Cdecl]<void*, byte*, void> HostReportFault;
        // Plugin-initiated user-facing toast — any thread. level matches NotifyLevel: 0=Info 1=Success
        // 2=Warning 3=Error. Shown verbatim (plugin name prefixed); not throttled.
        public delegate* unmanaged[Cdecl]<void*, int, byte*, void> HostNotify;
        // Axis & project state
        public delegate* unmanaged[Cdecl]<void*, int, void> SetActiveAxis;
        public delegate* unmanaged[Cdecl]<void*, int, int> IsAxisVisible;
        public delegate* unmanaged[Cdecl]<void*, int, int> IsAxisLocked;
        public delegate* unmanaged[Cdecl]<void*, int, byte*, int, int> GetAxisName;
        public delegate* unmanaged[Cdecl]<void*, int> IsProjectDirty;
        public delegate* unmanaged[Cdecl]<void*, byte*, int, int> GetProjectMetadata;
        public delegate* unmanaged[Cdecl]<void*, int> GetChapterCount;
        // trailing int* = nameReqOut (required name byte length, excl NUL); final int = found (1/0)
        public delegate* unmanaged[Cdecl]<void*, int, double*, double*, uint*, byte*, int, int*, int> GetChapter;
        public delegate* unmanaged[Cdecl]<void*, int> GetBookmarkCount;
        public delegate* unmanaged[Cdecl]<void*, int, double*, byte*, int, int*, int> GetBookmark;
        public delegate* unmanaged[Cdecl]<void*, int> GetRegionCount;
        public delegate* unmanaged[Cdecl]<void*, int, double*, double*, byte*, int, int*, int> GetRegion;
        // Localization — active UI language ISO 639 code ("en" = built-in English), fills buf, returns required len.
        public delegate* unmanaged[Cdecl]<void*, byte*, int, int> GetActiveLanguage;
        // Native dialogs (non-blocking) — onResult is an unmanaged callback the host invokes once on a
        // later frame; userData carries a GCHandle to the bridging TaskCompletionSource (see Dialogs.cs).
        public delegate* unmanaged[Cdecl]<void*, byte*, byte*, byte*,
            delegate* unmanaged[Cdecl]<void*, byte*, void>, void*, void> OpenFileDialog;
        // Multi-select open — onResult receives a byte** array of `count` UTF-8 path pointers (count 0 /
        // null when cancelled). userData carries the bridging GCHandle (see Dialogs.cs).
        public delegate* unmanaged[Cdecl]<void*, byte*, byte*, byte*,
            delegate* unmanaged[Cdecl]<void*, byte**, int, void>, void*, void> OpenFilesDialog;
        public delegate* unmanaged[Cdecl]<void*, byte*, byte*, byte*, byte*,
            delegate* unmanaged[Cdecl]<void*, byte*, void>, void*, void> SaveFileDialog;
        public delegate* unmanaged[Cdecl]<void*, byte*,
            delegate* unmanaged[Cdecl]<void*, byte*, void>, void*, void> PickFolder;
        public delegate* unmanaged[Cdecl]<void*, byte*, byte*, int,
            delegate* unmanaged[Cdecl]<void*, int, void>, void*, void> ConfirmDialog;
        // Immediate-mode UI (additions)
        public delegate* unmanaged[Cdecl]<void*, byte*, float*, int, int> UiColorEdit;
        public delegate* unmanaged[Cdecl]<void*, byte*, byte*, int, int, int, int> UiInputTextMultiline;
        public delegate* unmanaged[Cdecl]<void*, float, byte*, void> UiProgressBar;
        // Plugin-node custom UI state (main thread, only valid inside an onNodeUi call)
        public delegate* unmanaged[Cdecl]<void*, byte*, void> NodeUiSetState;
        public delegate* unmanaged[Cdecl]<void*, byte*> NodeUiGetState;
        // Current onNodeUi node identity (regionId << 32) | (uint32)nodeId; -1 outside such a call.
        public delegate* unmanaged[Cdecl]<void*, long> NodeUiCurrentKey;
        // Per-plugin project data (stored in the .ofp). Getter (key, buf, size) → required len; namespaced
        // by the calling plugin. Setter (key, jsonUtf8); empty/null json erases the key.
        public delegate* unmanaged[Cdecl]<void*, byte*, byte*, int, int> GetProjectData;
        public delegate* unmanaged[Cdecl]<void*, byte*, byte*, void> SetProjectData;
        // Per-plugin app data (global, stored in <pref>/plugin_settings/<plugin>.json). Getter (key, buf,
        // size) → required len; namespaced by the calling plugin. Setter (key, jsonUtf8); empty/null erases.
        public delegate* unmanaged[Cdecl]<void*, byte*, byte*, int, int> GetAppData;
        public delegate* unmanaged[Cdecl]<void*, byte*, byte*, void> SetAppData;
        // Interaction extension points — publish an edit mode / navigator (main thread, OnLoad).
        public delegate* unmanaged[Cdecl]<void*, OfsEditModeDef*, void> RegisterEditMode;
        public delegate* unmanaged[Cdecl]<void*, OfsNavigatorDef*, void> RegisterNavigator;
        // 1 if the calling plugin's edit mode / navigator localId is the active one (host prepends the
        // plugin name).
        public delegate* unmanaged[Cdecl]<void*, byte*, int> IsEditModeActive;
        public delegate* unmanaged[Cdecl]<void*, byte*, int> IsNavigatorActive;
        // Publish a selection mode (main thread, OnLoad); IsSelectionModeActive mirrors IsEditModeActive.
        public delegate* unmanaged[Cdecl]<void*, OfsSelectModeDef*, void> RegisterSelectionMode;
        public delegate* unmanaged[Cdecl]<void*, byte*, int> IsSelectionModeActive;
        // Word-wrapped paragraph text (vs UiLabel's single unwrapped line).
        public delegate* unmanaged[Cdecl]<void*, byte*, void> UiTextWrapped;
        // Scoped disabled block (greys + blocks input); nestable, host depth-guards the pop. 0 = no-op level.
        public delegate* unmanaged[Cdecl]<void*, int, void> UiPushDisabled;
        public delegate* unmanaged[Cdecl]<void*, void> UiPopDisabled;
        // Hover tooltip for the last-drawn widget (no-op if none precedes; never fires on a disabled widget).
        public delegate* unmanaged[Cdecl]<void*, byte*, void> UiTooltip;
        // Disabled block whose greyed widgets show `tooltip` on hover while disabled. Host copies the string.
        public delegate* unmanaged[Cdecl]<void*, int, byte*, void> UiPushDisabledTooltip;
        public delegate* unmanaged[Cdecl]<void*, void> UiPopDisabledTooltip;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct PluginApi
    {
        public int Version; // must equal ApiVersions.AbiVersion
        public IntPtr OnLoad;
        // Host calls this each frame to draw the plugin window
        public IntPtr OnBuildUI;
        // Optional event callbacks
        public IntPtr OnUpdate;
        public IntPtr OnTimeChange;
        public IntPtr OnPlayChange;
        public IntPtr OnSpeedChange;
        public IntPtr OnMediaChange;
        public IntPtr OnProjectChange;
        // role is a StandardAxis value cast to int
        public IntPtr OnAxisModified;
        // role is a StandardAxis value cast to int; there is always an active axis (never -1)
        public IntPtr OnActiveAxisChanged;
        // Fired when a command registered via RegisterCommand is invoked
        public IntPtr OnCommand;
        // Called before the plugin is unloaded
        public IntPtr OnUnload;
        // Returns the plugin's display name; pointer must remain valid for the plugin's lifetime
        public IntPtr GetName;
        // Returns the plugin's version; pointer must remain valid for the plugin's lifetime
        public IntPtr GetVersion;
    }
}
