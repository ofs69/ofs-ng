using System;
using System.Buffers;
using System.Text.Json;

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
        // Public fields are serialized too (a settings class is usually fields the UI binds to by ref).
        private static readonly JsonSerializerOptions Options = new() { IncludeFields = true };

        private readonly OfsHost _host;
        private readonly string _key;
        private T _value = new();

        // Change detection runs every frame (the host auto-flushes), so it must not allocate on the
        // no-change path. We serialize _value into a reused writer/buffer and compare the resulting UTF-8
        // bytes to the last persisted form: equal → return without touching the heap; different → copy the
        // new bytes and write them straight to native, skipping any JsonElement/string round-trip.
        private readonly ArrayBufferWriter<byte> _buffer = new();
        private readonly Utf8JsonWriter _writer;
        private byte[] _lastJson = Array.Empty<byte>(); // serialized form last loaded or saved

        internal AppScoped(OfsHost host, string key)
        {
            _host = host;
            _key = key;
            _writer = new Utf8JsonWriter(_buffer);
            Reload();
        }

        /// <summary>
        /// The live value. Mutate it freely (e.g. bind its fields into widgets); the host persists changes
        /// on the next frame's flush and on app close.
        /// </summary>
        public T Value => _value;

        // Persists Value if it changed since the last load/sync. Cheap and idempotent when nothing changed —
        // it allocates nothing on the no-change path. The host registers this to run automatically once per
        // frame (Host.FlushScopedValues); the native side caches the write and flushes to disk debounced.
        void IScopedValue.Sync()
        {
            ReadOnlySpan<byte> json = Serialize();
            if (json.SequenceEqual(_lastJson)) return;
            _lastJson = json.ToArray();
            _host.SetAppData(_key, json);
        }

        // Disposes the reused JSON writer. Called by the host on plugin unload (it owns this instance's lifetime).
        void IDisposable.Dispose() => _writer.Dispose();

        // Load the value from this plugin's stored settings, falling back to a fresh default when nothing is
        // stored under the key. Runs once at creation (app settings don't change mid-session).
        private void Reload()
        {
            JsonElement el = _host.GetAppData(_key);
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
