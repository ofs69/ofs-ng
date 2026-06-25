using System;
using System.Buffers;
using System.Text.Json;

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
        // Public fields are serialized too (a settings class is usually fields the UI binds to by ref).
        private static readonly JsonSerializerOptions Options = new() { IncludeFields = true };

        private readonly Project _project;
        private readonly string _key;
        private T _value = new();

        // Change detection runs every frame (the host auto-flushes), so it must not allocate on the
        // no-change path. We serialize _value into a reused writer/buffer and compare the resulting UTF-8
        // bytes to the last persisted form: equal → return without touching the heap; different → copy the
        // new bytes and write them straight to native, skipping any JsonElement/string round-trip.
        private readonly ArrayBufferWriter<byte> _buffer = new();
        private readonly Utf8JsonWriter _writer;
        private byte[] _lastJson = Array.Empty<byte>(); // serialized form last loaded or saved

        internal ProjectScoped(Project project, string key)
        {
            _project = project;
            _key = key;
            _writer = new Utf8JsonWriter(_buffer);
            Reload();
        }

        /// <summary>
        /// The live value for the current project. Mutate it freely (e.g. bind its fields into widgets);
        /// the host persists changes on the next frame's flush.
        /// </summary>
        public T Value => _value;

        // Persists Value into the project if it changed since the last load/sync. Cheap and idempotent when
        // nothing changed — it allocates nothing on the no-change path, so it never re-dirties the project.
        // The host registers this to run automatically once per frame (see Host.FlushScopedValues).
        void IScopedValue.Sync()
        {
            ReadOnlySpan<byte> json = Serialize();
            if (json.SequenceEqual(_lastJson)) return;
            _lastJson = json.ToArray();
            _project.SetData(_key, json);
        }

        // Disposes the reused JSON writer. Called by the host on plugin unload (it owns this instance's lifetime).
        void IDisposable.Dispose() => _writer.Dispose();

        // Load the value from the current project, falling back to a fresh default when the project stores
        // nothing under the key. Wired to Project-changed so each opened project gets its own value.
        internal void Reload()
        {
            JsonElement el = _project.GetData(_key);
            _value = el.ValueKind == JsonValueKind.Undefined ? new T() : el.Deserialize<T>(Options) ?? new T();
            _lastJson = Serialize().ToArray(); // baseline so the first Sync after a load is a no-op
        }

        // Serialize _value into the reused buffer; returns the written UTF-8 span, valid until the next call.
        private ReadOnlySpan<byte> Serialize()
        {
            _buffer.Clear();
            _writer.Reset();
            JsonSerializer.Serialize(_writer, _value, Options);
            return _buffer.WrittenSpan;
        }
    }
}
