using System;

namespace Ofs
{
    /// <summary>
    /// A value of type <typeparamref name="T"/> that lives in this plugin's global settings: it is loaded
    /// once when created and saved back whenever it changes. Get it once in <see cref="OfsPlugin.OnLoad"/>
    /// via <see cref="IOfsHost.AppScoped{T}(string)"/>, then read and mutate <see cref="Value"/> directly
    /// (its fields/properties bind straight into <see cref="Ui"/> widgets) — the host flushes edits once
    /// per frame and again on app close, so there is nothing to call.
    /// <para>Unlike <see cref="ProjectScoped{T}"/>, this is global to all projects (it does not reset or
    /// reload when a different project is opened); it is persisted in this plugin's own file under the
    /// app's settings directory. <typeparamref name="T"/> is JSON-serialized including public fields, so a
    /// plain settings class with public fields works without attributes. Main-thread only.</para>
    /// </summary>
    public sealed class AppScoped<T> : IScopedValue where T : new()
    {
        // The shared serialize/change-detect/persist engine, bound to this plugin's app-settings store.
        private readonly ScopedStore<T> _store;

        internal AppScoped(OfsHost host, string key)
            => _store = new ScopedStore<T>(() => host.GetAppData(key), bytes => host.SetAppData(key, bytes));

        /// <summary>
        /// The live value. Mutate it freely (e.g. bind its fields into widgets); the host persists changes
        /// on the next frame's flush and on app close.
        /// </summary>
        public T Value => _store.Value;

        // The host registers this to run automatically once per frame (Host.FlushScopedValues); the native
        // side caches the write and flushes to disk debounced.
        void IScopedValue.Sync() => _store.Sync();

        // Called by the host on plugin unload (it owns this instance's lifetime).
        void IDisposable.Dispose() => _store.Dispose();
    }
}
