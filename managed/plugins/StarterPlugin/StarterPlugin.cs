using Ofs;

namespace StarterPlugin;

/// <summary>
/// A minimal ofs-ng plugin to start from. Derive from <see cref="OfsPlugin"/>,
/// give it a <see cref="Name"/>, and override only the hooks you need.
/// </summary>
public sealed class StarterPlugin : OfsPlugin
{
    // Shown in the plugin list and as the window title. Returning a Str.* getter makes it follow ofs-ng's
    // UI language too — the host re-pulls Name on a language switch. (A literal string here is fine if you
    // don't localize.)
    public override string Name => Str.PluginName;

    private int _clicks;

    /// <summary>Called once when the plugin loads. Register shortcuts, nodes and
    /// event subscriptions here.</summary>
    protected override void OnLoad()
    {
        Host.Log($"{Name} loaded.");
    }

    /// <summary>Called every frame on the main thread to draw this plugin's window.
    /// Remove this override if the plugin has no UI.</summary>
    protected override void OnRenderUi(Ui ui)
    {
        // Str.* are strongly-typed getters generated from Strings.resx; they resolve in ofs-ng's active
        // language automatically (the base keeps them synced — no wiring). Localization is optional:
        // delete Strings.resx and pass literal strings to ui.* directly instead. See README "Localization".
        // Format with Host.Culture so the number renders in ofs-ng's active language, matching the catalog text.
        ui.Label(string.Format(Host.Culture, Str.TimeFmt, Host.Player.Time));

        if (ui.Button(Str.ClickMe))
            _clicks++;

        ui.Label(string.Format(Host.Culture, Str.ClicksFmt, _clicks));
    }
}
