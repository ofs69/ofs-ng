using System;
using System.Buffers;
using System.Text.Json;

namespace Ofs
{
    // Shared serialize/change-detect/persist engine behind AppScoped<T> and ProjectScoped<T>. Those two
    // differ only in their backing store (this plugin's app settings vs the current project), so the load
    // and store calls are injected as delegates and everything else lives here once. Internal — the two
    // public scoped wrappers compose one each and forward to it (a public class cannot derive from an
    // internal base, so this is composition rather than a shared base class). Main-thread only.
    internal sealed class ScopedStore<T> : IDisposable where T : new()
    {
        // Public fields are serialized too (a settings class is usually fields the UI binds to by ref).
        private static readonly JsonSerializerOptions Options = new() { IncludeFields = true };

        private readonly Func<JsonElement> _load;       // read the stored value (Undefined when nothing stored)
        private readonly Action<byte[]> _store;         // persist the serialized UTF-8 bytes to the backing store
        private T _value = new();

        // Change detection runs every frame (the host auto-flushes), so it must not allocate on the
        // no-change path. We serialize _value into a reused writer/buffer and compare the resulting UTF-8
        // bytes to the last persisted form: equal → return without touching the heap; different → copy the
        // new bytes and write them straight to native, skipping any JsonElement/string round-trip.
        private readonly ArrayBufferWriter<byte> _buffer = new();
        private readonly Utf8JsonWriter _writer;
        private byte[] _lastJson = Array.Empty<byte>(); // serialized form last loaded or saved

        internal ScopedStore(Func<JsonElement> load, Action<byte[]> store)
        {
            _load = load;
            _store = store;
            _writer = new Utf8JsonWriter(_buffer);
            Reload();
        }

        internal T Value => _value;

        // Persist Value if it changed since the last load/sync. Cheap and idempotent when nothing changed —
        // it allocates nothing on the no-change path, so it never re-dirties the project / settings.
        internal void Sync()
        {
            ReadOnlySpan<byte> json = Serialize();
            if (json.SequenceEqual(_lastJson)) return;
            byte[] copy = json.ToArray();
            _lastJson = copy;
            _store(copy);
        }

        // Disposes the reused JSON writer. Called by the owning wrapper on plugin unload.
        public void Dispose() => _writer.Dispose();

        // Load the value from the backing store, falling back to a fresh default when nothing is stored
        // under the key. Used as the ProjectScoped project-changed handler, so each opened project gets its
        // own value; AppScoped calls it once at creation.
        internal void Reload()
        {
            JsonElement el = _load();
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
