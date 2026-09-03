# Classic OFS WebSocket API

ofs-ng provides a loopback WebSocket endpoint compatible with applications that consume the classic
OpenFunscripter JSON protocol, including MultiFunPlayer's OFS source.

## Enable the endpoint

Open **View → WebSocket API**, choose a port while the server is stopped, and enable **Server active**.
The default endpoint is:

```text
ws://127.0.0.1:8080/ofs
```

The server is disabled by default. Its enabled state and port are stored in `settings.json`. It binds only
to IPv4 loopback and never accepts connections through a LAN interface. Up to 16 clients may connect at
once.

The optional WebSocket subprotocol is `ofs-api.json`. Clients that do not request a subprotocol are also
accepted for compatibility. Messages are UTF-8 JSON text frames; fragmented text frames are supported.

> **Security:** The endpoint has no authentication and does not validate the HTTP `Origin` header. Enabling
> it allows local processes—and browser pages permitted to reach loopback by the browser's network
> policy—to read the current project/script state and control playback. Leave it disabled when no local
> integration needs it.

## Client commands

Commands use this envelope:

```json
{
  "type": "command",
  "name": "change_time",
  "data": { "time": 12.5 }
}
```

Supported commands are:

| `name` | `data` | Effect |
|---|---|---|
| `change_time` | `{ "time": number }` | Seek to a non-negative time in seconds. |
| `change_play` | `{ "playing": boolean }` | Start or pause playback. |
| `change_playbackspeed` | `{ "speed": number }` | Set a positive, finite playback-speed multiplier representable as a 32-bit float. |

Unknown or malformed commands are ignored. A bad command does not close the connection.

## Server messages

Immediately after connecting, a client receives a `connected` object followed by a complete state snapshot:

```json
{ "connected": "OFS <version>" }
```

State changes then use this event envelope:

```json
{
  "type": "event",
  "name": "time_change",
  "data": { "time": 12.5 }
}
```

| `name` | `data` | When sent |
|---|---|---|
| `project_change` | `{}` | The active project changes or a full state refresh is required. |
| `media_change` | `{ "path": string }` | The active media changes. Paths are UTF-8. |
| `playbackspeed_change` | `{ "speed": number }` | Playback speed changes. |
| `play_change` | `{ "playing": boolean }` | Playback starts or pauses. |
| `duration_change` | `{ "duration": number }` | Media or dummy duration changes, in seconds. |
| `time_change` | `{ "time": number }` | The playback cursor moves, in seconds. |
| `funscript_change` | `{ "name": string, "funscript": object }` | An axis is introduced or its script data changes. Rapid edits are debounced for 200 ms. |
| `funscript_remove` | `{ "name": string }` | A previously announced axis no longer exists. |

`funscript_change.data.funscript` is a standard funscript document containing actions plus the current
metadata, bookmarks, chapters, and duration. `name` is the script's Classic OFS *title*, not a file name:
the primary stroke axis uses `<media-name>` and every other standard axis `<media-name>.<axis-tag>`
(e.g. `video` and `video.R1`), with **no `.funscript` extension**. Clients map a script to an axis by the
last dot-separated segment of the name, so an extension there hides the tag from them.

## Protocol limits

- HTTP upgrade headers: 64 KiB maximum.
- One WebSocket payload: 16 MiB maximum.
- Buffered input per client: 32 MiB maximum.
- Queued output per client: 64 MiB maximum.

Clients must mask frames as required by RFC 6455. Protocol violations or exceeded limits close that client
connection without stopping the server.
