using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

namespace Ofs
{
    /// <summary>
    /// Command registration. Handlers are supplied inline — there is no central dispatch switch.
    /// Call <see cref="Register"/> from OnLoad.
    /// </summary>
    public sealed unsafe class Commands
    {
        private readonly OfsHost _host;
        private readonly HostApi* _api;

        private readonly Dictionary<string, Action> _handlers = new();

        // Mirror of OfsRegisterCommandResult in PluginApi.h — distinct failure codes so a failed
        // registration reports its real reason, not a blanket "id already taken". Keep in sync.
        private enum RegisterResult
        {
            Ok = 0,
            WrongThread = 1,
            NotOnLoad = 2,
            InvalidArg = 3,
            NotInvokable = 4,
            DuplicateId = 5,
        }

        internal Commands(OfsHost host, HostApi* api)
        {
            _host = host;
            _api = api;
        }

        /// <summary>
        /// Registers a palette command. Call from OnLoad.
        /// </summary>
        /// <param name="id">Local id (no namespace prefix); host prepends plugin name.</param>
        /// <param name="title">Display name shown in the command palette.</param>
        /// <param name="handler">Action to run when the command is invoked.</param>
        /// <param name="inRebindList">If true, the command is offered for binding in the Shortcut window by
        /// default. Defaults to false (palette-only) — a user can still opt in to bind a palette-only command.</param>
        /// <param name="inPalette">If true, the command is searchable in the command palette.</param>
        /// <exception cref="ArgumentException">Thrown if the command is listed on neither surface (rebind UI nor palette) — the user could never discover it to bind or invoke.</exception>
        /// <remarks>To localize the title, build it from your own catalog at OnLoad (e.g. <c>Str.MyCommand</c>):
        /// the host reloads every plugin on a UI-language switch, so OnLoad re-runs and re-registers it.</remarks>
        public void Register(string id, string title, Action handler, bool inRebindList = false, bool inPalette = true)
        {
            _host.AssertMainThread(nameof(Register));
            _host.AssertOnLoad(nameof(Register));
            ArgumentNullException.ThrowIfNull(handler);
            if (string.IsNullOrEmpty(id)) throw new ArgumentException("Command id must be non-empty.", nameof(id));
            string titleText = title ?? string.Empty;
            if (string.IsNullOrEmpty(titleText)) throw new ArgumentException("Command title must be non-empty.", nameof(title));
            if (!inRebindList && !inPalette)
                throw new ArgumentException("A command must be in the rebind list or the palette; otherwise the user can never discover it to invoke.", nameof(inPalette));

            byte[] idBytes = Encoding.UTF8.GetBytes(id + "\0");
            byte[] titleBytes = Encoding.UTF8.GetBytes(titleText + "\0");
            int result;
            fixed (byte* idPtr = idBytes)
            fixed (byte* titlePtr = titleBytes)
            {
                var def = new OfsCommandDef
                {
                    Id = idPtr,
                    Group = null,
                    Title = titlePtr,
                    InRebindList = inRebindList ? 1 : 0,
                    InPalette = inPalette ? 1 : 0
                };
                result = _api->RegisterCommand(_api->Ctx, &def);
            }
            switch ((RegisterResult)result)
            {
                case RegisterResult.Ok:
                    break;
                case RegisterResult.DuplicateId:
                    throw new InvalidOperationException($"RegisterCommand: command id '{id}' is already registered.");
                case RegisterResult.NotInvokable:
                    throw new ArgumentException($"RegisterCommand: command '{id}' is listed in neither the rebind list nor the palette.");
                case RegisterResult.InvalidArg:
                    throw new ArgumentException($"RegisterCommand: command '{id}' has an empty id or title.");
                case RegisterResult.WrongThread:
                    throw new InvalidOperationException($"RegisterCommand: '{id}' must be registered on the main thread.");
                case RegisterResult.NotOnLoad:
                    throw new InvalidOperationException($"RegisterCommand: '{id}' must be registered from OnLoad (not at runtime).");
                default:
                    throw new InvalidOperationException($"RegisterCommand: failed to register '{id}' (code {result}).");
            }

            _handlers[id] = handler;
        }

        internal void Dispatch(string localId)
        {
            if (_handlers.TryGetValue(localId, out var h)) h();
        }

        // Drop all plugin command handlers on unload — they capture the plugin instance and would
        // otherwise root its collectible ALC. See OfsHost.DropPluginDelegates.
        internal void ClearHandlers()
        {
            _handlers.Clear();
        }
    }
}
