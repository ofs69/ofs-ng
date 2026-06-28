namespace Ofs
{
    /// <summary>
    /// Base class for every ofs-ng plugin. Derive from it and override only what you need.
    /// The single required member is <see cref="Name"/>.
    /// </summary>
    public abstract class OfsPlugin
    {
        /// <summary>Display name shown in the plugin list. Required.</summary>
        public abstract string Name { get; }

        /// <summary>The host. Available from <see cref="OnLoad"/> onward; never null inside callbacks.</summary>
        protected IOfsHost Host { get; private set; } = null!;

        internal void SetHost(IOfsHost host)
        {
            Host = host;
            // If this plugin ships a localized resx (its generated Strings class), keep that class's getters
            // pointed at ofs-ng's language picker — automatically, so authors never wire culture by hand.
            Localization.Wire(GetType().Assembly, host);
        }

        /// <summary>Called once after the plugin is constructed and the host is wired.
        /// Register shortcuts, nodes, and event subscriptions here.</summary>
        protected virtual void OnLoad() { }

        /// <summary>Called once before the plugin is unloaded. Stop threads, dispose resources. Event
        /// subscriptions made through Host are dropped automatically. Any background work that outlives this
        /// call must observe <see cref="IOfsHost.UnloadToken"/> (cancelled right after OnUnload returns) —
        /// a thread or timer left running will keep the plugin assembly loaded and its DLL locked.</summary>
        protected virtual void OnUnload() { }

        /// <summary>Called every frame on the main thread to draw the plugin window.
        /// Override only if the plugin has UI. Not overriding it means "no window".</summary>
        protected virtual void OnRenderUi(Ui ui) { }

        /// <summary>Called every frame on the main thread before rendering. Override for
        /// per-frame logic. Queued work from background threads runs before this.</summary>
        protected virtual void OnUpdate(float deltaSeconds) { }

        // Internal invokers — the bridge lives in the same assembly and calls these,
        // since the lifecycle methods themselves are protected.
        internal void InvokeOnLoad() => OnLoad();
        internal void InvokeOnUnload() => OnUnload();
        internal void InvokeOnRenderUi(Ui ui) => OnRenderUi(ui);
        internal void InvokeOnUpdate(float dt) => OnUpdate(dt);
    }
}
