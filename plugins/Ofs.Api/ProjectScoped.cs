using System;

namespace Ofs
{
    /// <summary>
    /// A value of type <typeparamref name="T"/> that lives in the current project: it is loaded from the
    /// project file on creation, automatically reloaded whenever a different project is opened, and saved
    /// back when it changes. Get it once in <see cref="OfsPlugin.OnLoad"/> via
    /// <see cref="Project.Scoped{T}(string)"/>, then read and mutate <see cref="Value"/> directly (its
    /// fields/properties bind straight into <see cref="Ui"/> widgets) — the host flushes edits once per
    /// frame, so there is nothing to call. A project with nothing stored under the key resets
    /// <see cref="Value"/> to a fresh <c>new T()</c>, so settings never bleed between projects.
    /// <para><typeparamref name="T"/> is JSON-serialized including public fields, so a plain settings class
    /// with public fields works without attributes. Main-thread only.</para>
    /// </summary>
    public sealed class ProjectScoped<T> : IScopedValue where T : new()
    {
        // The shared serialize/change-detect/persist engine, bound to the current project's data store.
        private readonly ScopedStore<T> _store;

        internal ProjectScoped(Project project, string key)
            => _store = new ScopedStore<T>(() => project.GetData(key), bytes => project.SetData(key, bytes));

        /// <summary>
        /// The live value for the current project. Mutate it freely (e.g. bind its fields into widgets);
        /// the host persists changes on the next frame's flush.
        /// </summary>
        public T Value => _store.Value;

        // Wired to the host's Project-changed event so each opened project gets its own value.
        internal void Reload() => _store.Reload();

        // The host registers this to run automatically once per frame (see Host.FlushScopedValues).
        void IScopedValue.Sync() => _store.Sync();

        // Called by the host on plugin unload (it owns this instance's lifetime).
        void IDisposable.Dispose() => _store.Dispose();
    }
}
