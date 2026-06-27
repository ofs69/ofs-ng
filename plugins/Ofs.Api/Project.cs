using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;

namespace Ofs
{
    /// <summary>A named chapter: a time range with a color.</summary>
    public readonly record struct ProjectChapter(double Start, double End, uint Color, string Name);

    /// <summary>A point-in-time bookmark.</summary>
    public readonly record struct ProjectBookmark(double Time, string Name);

    /// <summary>A processing region: a named time range with its own node graph.</summary>
    public readonly record struct ProjectRegion(double Start, double End, string Name);

    /// <summary>
    /// Access to project-level state: unsaved status, metadata, chapters, bookmarks and processing
    /// regions (all read-only snapshots), plus this plugin's own per-project data store
    /// (<see cref="Scoped{T}(string)"/>), which is persisted inside the .ofp and round-trips with
    /// save/load. All members are main-thread only and read fresh each call — the returned lists are
    /// snapshots, not live views.
    /// </summary>
    public sealed unsafe class Project
    {
        private readonly OfsHost _host;
        private readonly HostApi* _api;

        // Stack-buffer size for the common case where a chapter/bookmark/region name fits inline; a longer
        // name overflows it and is re-read exactly via OfsHost.RereadName.
        private const int NameBufSize = 256;

        internal Project(OfsHost host, HostApi* api)
        {
            _host = host;
            _api = api;
        }

        // Reads record `i`'s scalar fields (stashed by the caller's closure) plus its name into `nameBuf`,
        // reporting the name's required length in `*nameReq`. Returns 0 to skip the record. The same reader
        // serves the right-sized re-read on overflow, so it must be idempotent.
        private delegate int ReadRecord(int i, byte* nameBuf, int bufSize, int* nameReq);

        // Shared count/loop/stack-buffer/overflow-reread for the name-bearing record getters (Chapters /
        // Bookmarks / Regions). For each of `count` records `read` fills the reused stack buffer; the name
        // is taken inline when it fits, else re-read exactly against a right-sized buffer. `build(name)`
        // turns the scalars `read` stashed plus the resolved name into the element.
        private List<T> ReadNamedRecords<T>(int count, ReadRecord read, Func<string, T> build)
        {
            var list = new List<T>(count > 0 ? count : 0);
            byte* nameBuf = stackalloc byte[NameBufSize]; // reused each iteration; host re-NUL-terminates per call
            for (int i = 0; i < count; i++)
            {
                int nameReq;
                if (read(i, nameBuf, NameBufSize, &nameReq) == 0) continue;
                int idx = i; // capture by value for the overflow reread closure
                string name = nameReq < NameBufSize
                    ? Marshal.PtrToStringUTF8((IntPtr)nameBuf) ?? string.Empty
                    : OfsHost.RereadName(nameReq, (buf, size) => { int nr; return read(idx, buf, size, &nr); });
                list.Add(build(name));
            }
            return list;
        }

        /// <summary>True if the project has unsaved changes.</summary>
        public bool IsDirty
        {
            get { _host.AssertMainThread(nameof(IsDirty)); return _api->IsProjectDirty(_api->Ctx) != 0; }
        }

        /// <summary>
        /// Full project metadata as a <see cref="JsonElement"/> (same schema as the .funscript metadata
        /// block): title, creator, scriptUrl, videoUrl, description, notes, tags, performers, license,
        /// plus any custom fields. Returns an empty <see cref="JsonElement"/> when no project is loaded.
        /// Main-thread only.
        /// </summary>
        public JsonElement Metadata
        {
            get
            {
                _host.AssertMainThread(nameof(Metadata));
                string json = OfsHost.GrowAndRead((buf, size) => _api->GetProjectMetadata(_api->Ctx, buf, size));
                if (string.IsNullOrEmpty(json)) return default;
                using var doc = JsonDocument.Parse(json);
                return doc.RootElement.Clone(); // Clone owns its memory independently of the document
            }
        }

        /// <summary>The chapters, in project order (by start time).</summary>
        public IReadOnlyList<ProjectChapter> Chapters
        {
            get
            {
                _host.AssertMainThread(nameof(Chapters));
                double start = 0, end = 0; uint color = 0;
                return ReadNamedRecords(_api->GetChapterCount(_api->Ctx),
                    (int i, byte* buf, int size, int* nameReq) =>
                    {
                        double s, e; uint c;
                        int ok = _api->GetChapter(_api->Ctx, i, &s, &e, &c, buf, size, nameReq);
                        start = s; end = e; color = c;
                        return ok;
                    },
                    name => new ProjectChapter(start, end, color, name));
            }
        }

        /// <summary>The bookmarks, in project order (by time).</summary>
        public IReadOnlyList<ProjectBookmark> Bookmarks
        {
            get
            {
                _host.AssertMainThread(nameof(Bookmarks));
                double time = 0;
                return ReadNamedRecords(_api->GetBookmarkCount(_api->Ctx),
                    (int i, byte* buf, int size, int* nameReq) =>
                    {
                        double t;
                        int ok = _api->GetBookmark(_api->Ctx, i, &t, buf, size, nameReq);
                        time = t;
                        return ok;
                    },
                    name => new ProjectBookmark(time, name));
            }
        }

        /// <summary>The processing regions, in project order (always sorted by start time).</summary>
        public IReadOnlyList<ProjectRegion> Regions
        {
            get
            {
                _host.AssertMainThread(nameof(Regions));
                double start = 0, end = 0;
                return ReadNamedRecords(_api->GetRegionCount(_api->Ctx),
                    (int i, byte* buf, int size, int* nameReq) =>
                    {
                        double s, e;
                        int ok = _api->GetRegion(_api->Ctx, i, &s, &e, buf, size, nameReq);
                        start = s; end = e;
                        return ok;
                    },
                    name => new ProjectRegion(start, end, name));
            }
        }

        // ── Per-plugin project data ──────────────────────────────────────────────────────────────
        // A key→value store private to this plugin, persisted inside the project file (.ofp) and so
        // round-tripped with save/load — unlike Host.AppScoped, which is global to all projects.
        // The host namespaces by plugin: a plugin only ever sees its own keys. Writing marks the project
        // dirty; the data is not part of undo/redo. Main-thread only.
        //
        // The store is reached only through Scoped<T>: it holds the value in memory, reloads it on project
        // open, and saves on change. The raw GetData/SetData below are internal — the building blocks
        // ProjectScoped uses — and are deliberately not part of the plugin surface.
        //
        // Reads and writes are asymmetric: GetData reads the live store synchronously, but SetData is
        // deferred (it queues the change, applied on the next host tick). So GetData immediately after a
        // SetData still returns the OLD value; ProjectScoped<T> sidesteps this by keeping its own
        // in-memory copy.

        // This plugin's stored value for the key as a JsonElement, or an empty element
        // (ValueKind == Undefined) when nothing is stored under it. Main-thread only.
        internal JsonElement GetData(string key)
        {
            _host.AssertMainThread(nameof(GetData));
            if (string.IsNullOrEmpty(key)) return default;
            byte[] keyBytes = Encoding.UTF8.GetBytes(key + "\0");
            string json = OfsHost.GrowAndRead((buf, size) =>
            {
                fixed (byte* keyPtr = keyBytes)
                    return _api->GetProjectData(_api->Ctx, keyPtr, buf, size);
            });
            if (string.IsNullOrEmpty(json)) return default;
            using var doc = JsonDocument.Parse(json);
            return doc.RootElement.Clone(); // Clone owns its memory independently of the document
        }

        // Raw UTF-8 JSON write for ProjectScoped.Sync, which already holds the serialized value in its reused
        // buffer: this writes only the two native-bound byte arrays, never a JsonDocument. The write is
        // deferred (applied on the next host tick) and an empty payload erases the key. Main-thread only.
        internal void SetData(string key, ReadOnlySpan<byte> utf8Json)
        {
            _host.AssertMainThread(nameof(SetData));
            if (string.IsNullOrEmpty(key)) return;
            byte[] keyBytes = Encoding.UTF8.GetBytes(key + "\0");
            byte[] valBytes = new byte[utf8Json.Length + 1]; // + NUL: the host reads a C string
            utf8Json.CopyTo(valBytes);
            fixed (byte* keyPtr = keyBytes)
            fixed (byte* valPtr = valBytes)
                _api->SetProjectData(_api->Ctx, keyPtr, valPtr);
        }

        /// <summary>
        /// A <see cref="ProjectScoped{T}"/> handle over this plugin's data at <paramref name="key"/>: it
        /// loads the value now, reloads it whenever a different project is opened, and saves it back on
        /// change — so a plugin's per-project state is one field with no manual load/save/reset wiring.
        /// Create it once in <see cref="OfsPlugin.OnLoad"/>; the host flushes edits once per frame, so there
        /// is nothing manual to call. Main-thread only.
        /// </summary>
        public ProjectScoped<T> Scoped<T>(string key) where T : new()
        {
            _host.AssertMainThread(nameof(Scoped));
            var scoped = new ProjectScoped<T>(this, key);
            // Reload on every project open and flush edits once per frame; the host drops both on unload
            // (Axes.ClearSubscribers / FlushScopedValues clear) so the plugin needs no manual Sync or teardown.
            _host.Axes.ProjectChanged += scoped.Reload;
            _host.RegisterScopedFlush(scoped);
            return scoped;
        }
    }
}
