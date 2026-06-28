using System;
using System.Collections.Generic;
using System.Globalization;
using System.Reflection;
using System.Resources;

namespace Ofs
{
    // Points a plugin's generated resx accessor at ofs-ng's UI language, so its strongly-typed getters
    // (e.g. Str.MyKey) resolve in the in-app picker's language with zero wiring in the plugin. Called by
    // OfsPlugin the moment the host is set, before OnLoad. This is a one-time set: the host reloads every
    // plugin on a UI-language switch, so Wire runs afresh (in the new language) on each reload.
    //
    // ofs-ng renders plugin-supplied strings verbatim — it does NOT translate them — so localizing a
    // plugin is the plugin's own job and entirely optional. To localize, a plugin ships a neutral
    // Strings.resx plus one Strings.<culture>.resx per language and enables build-time strongly-typed
    // generation (see the StarterPlugin .csproj / README), which emits a resource accessor class.
    //
    // Those getters would otherwise key off CultureInfo.CurrentUICulture (the OS UI culture, independent
    // of ofs-ng's own picker). We point the accessor's static Culture at Host.Culture — redirecting the
    // lookups without touching the process-wide culture (which would also reskin the host).
    //
    // The accessor is found by shape, not by name: a generated resx class is the (only) type carrying both
    // a static ResourceManager and a settable static Culture. So the plugin may name the class anything
    // (StronglyTypedClassName in its .csproj) and the sync still works — no fragile name convention, and
    // no silent fall-back to the OS language if it's renamed. No-op for a plugin that ships no such class.
    internal static class Localization
    {
        public static void Wire(Assembly pluginAssembly, IOfsHost host)
        {
            var setters = FindCultureSetters(pluginAssembly);
            if (setters.Count == 0) return; // plugin not localized — nothing to sync

            foreach (var set in setters) set(host.Culture);
        }

        private static List<Action<CultureInfo>> FindCultureSetters(Assembly asm)
        {
            var setters = new List<Action<CultureInfo>>();

            Type?[] types;
            try { types = asm.GetTypes(); }
            catch (ReflectionTypeLoadException ex) { types = ex.Types; } // keep the types that did load

            const BindingFlags flags = BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic;
            foreach (var t in types)
            {
                if (t is null) continue;
                var rm = t.GetProperty("ResourceManager", flags);
                if (rm is null || rm.PropertyType != typeof(ResourceManager)) continue;
                var culture = t.GetProperty("Culture", flags);
                if (culture is null || !culture.CanWrite || culture.PropertyType != typeof(CultureInfo)) continue;
                setters.Add(c => culture.SetValue(null, c));
            }
            return setters;
        }
    }
}
