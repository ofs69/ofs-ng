using System;
using System.Runtime.InteropServices;

namespace Ofs
{
    /// <summary>
    /// Playback state, commands and events. State is exposed as properties; commands are
    /// methods; notifications are events. All members must be touched on the main thread.
    /// </summary>
    public sealed unsafe class Player
    {
        private readonly OfsHost _host;
        private readonly HostApi* _api;

        internal Player(OfsHost host, HostApi* api)
        {
            _host = host;
            _api = api;
        }

        /// <summary>Current playback position, seconds.</summary>
        public double Time
        {
            get { _host.AssertMainThread(nameof(Time)); return _api->GetTime(_api->Ctx); }
        }

        /// <summary>Media length, seconds.</summary>
        public double Duration
        {
            get { _host.AssertMainThread(nameof(Duration)); return _api->GetDuration(_api->Ctx); }
        }

        private string? _mediaPathCache;

        /// <summary>Path of the loaded media, or "" if none.</summary>
        public string MediaPath
        {
            get
            {
                _host.AssertMainThread(nameof(MediaPath));
                // Refreshed on MediaChanged (RaiseMediaChanged) from the event payload. The first read before
                // any event falls through to a native read with grow-and-retry (no silent truncation).
                return _mediaPathCache ??= OfsHost.GrowAndRead((buf, size) => _api->GetMediaPath(_api->Ctx, buf, size));
            }
        }

        /// <summary>Whether playback is running. Set to play/pause.</summary>
        public bool IsPlaying
        {
            get { _host.AssertMainThread(nameof(IsPlaying)); return _api->IsPlaying(_api->Ctx) != 0; }
            set { _host.AssertMainThread(nameof(IsPlaying)); _api->SetPlaying(_api->Ctx, value ? 1 : 0); }
        }

        /// <summary>Playback rate, e.g. 1.0.</summary>
        public float Speed
        {
            get { _host.AssertMainThread(nameof(Speed)); return _api->GetSpeed(_api->Ctx); }
            set { _host.AssertMainThread(nameof(Speed)); _api->SetSpeed(_api->Ctx, value); }
        }

        /// <summary>Playback volume, 0..1. Reads 0 when no media is loaded.</summary>
        public float Volume
        {
            get { _host.AssertMainThread(nameof(Volume)); return _api->GetVolume(_api->Ctx); }
            set { _host.AssertMainThread(nameof(Volume)); _api->SetVolume(_api->Ctx, value); }
        }

        /// <summary>Media frame rate, or null when no media is loaded.</summary>
        public double? Fps
        {
            get
            {
                _host.AssertMainThread(nameof(Fps));
                double fps = _api->GetFps(_api->Ctx);
                return fps > 0.0 ? fps : (double?)null;
            }
        }

        /// <summary>Video frame width in pixels, or null when no video is loaded.</summary>
        public int? VideoWidth
        {
            get
            {
                _host.AssertMainThread(nameof(VideoWidth));
                int w = _api->GetVideoWidth(_api->Ctx);
                return w > 0 ? w : (int?)null;
            }
        }

        /// <summary>Video frame height in pixels, or null when no video is loaded.</summary>
        public int? VideoHeight
        {
            get
            {
                _host.AssertMainThread(nameof(VideoHeight));
                int h = _api->GetVideoHeight(_api->Ctx);
                return h > 0 ? h : (int?)null;
            }
        }

        /// <summary>Seeks the playhead to <paramref name="time"/> seconds. Main-thread only.</summary>
        public void Seek(double time)
        {
            _host.AssertMainThread(nameof(Seek));
            _api->SeekTo(_api->Ctx, time);
        }

        /// <inheritdoc cref="Seek(double)"/>
        public void Seek(ScriptAction action) => Seek(action.At);

        /// <summary>Toggles between play and pause. Main-thread only.</summary>
        public void TogglePlay()
        {
            _host.AssertMainThread(nameof(TogglePlay));
            _api->SetPlaying(_api->Ctx, _api->IsPlaying(_api->Ctx) != 0 ? 0 : 1);
        }

        /// <summary>Raised as playback advances; the argument is the new time in seconds.</summary>
        public event Action<double>? TimeChanged;
        /// <summary>Raised when playback starts or stops; the argument is true while playing.</summary>
        public event Action<bool>? PlayingChanged;
        /// <summary>Raised when the playback speed changes; the argument is the new rate (1.0 = normal).</summary>
        public event Action<float>? SpeedChanged;
        /// <summary>Raised when a different media file loads; the argument is its path.</summary>
        public event Action<string>? MediaChanged;

        internal void RaiseTimeChanged(double t) => TimeChanged?.Invoke(t);
        internal void RaisePlayingChanged(bool p) => PlayingChanged?.Invoke(p);
        internal void RaiseSpeedChanged(float s) => SpeedChanged?.Invoke(s);
        internal void RaiseMediaChanged(string m) { _mediaPathCache = m; MediaChanged?.Invoke(m); }

        // Drop all plugin subscriptions on unload. The handler delegates capture the plugin instance, so
        // (Ofs.Api being shared with the host's non-collectible context) they would otherwise root the
        // plugin's collectible ALC. See OfsHost.DropPluginDelegates.
        internal void ClearSubscribers()
        {
            TimeChanged = null;
            PlayingChanged = null;
            SpeedChanged = null;
            MediaChanged = null;
        }
    }
}
