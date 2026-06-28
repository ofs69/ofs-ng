using System;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Runtime.Loader;
using System.Text;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Ofs;

namespace Ofs.ScriptHost
{
    // Native↔managed result of a compile. Layout must match OfsScriptCompileResult in
    // src/Services/ScriptSystem.h.
    [StructLayout(LayoutKind.Sequential)]
    internal struct ScriptCompileResult
    {
        public IntPtr Fn;       // trampoline (OfsGenericFn)
        public IntPtr UserData; // slot index
        public int Signal;      // OfsSignalKind
    }

    // A compiled script assembly loads here so it could be unloaded; like the plugin loader, the
    // host's Ofs.Api is shared rather than reloaded (so the script binds against the same slot
    // tables / public types the eval trampolines use).
    internal sealed class ScriptLoadContext : AssemblyLoadContext
    {
        public ScriptLoadContext() : base(isCollectible: true) { }

        protected override Assembly? Load(AssemblyName assemblyName)
        {
            if (assemblyName.Name == "Ofs.Api")
            {
                var apiAlc = GetLoadContext(typeof(NodeContext).Assembly);
                return apiAlc?.Assemblies.FirstOrDefault(a => a.GetName().Name == "Ofs.Api");
            }
            return null; // everything else resolves from the default context
        }
    }

    public static class ScriptCompiler
    {
        // Make the discrete I/O accessor table available to the discrete trampolines even when no
        // plugin registered a discrete node. The HostApi pointer must outlive every evaluation.
        [UnmanagedCallersOnly]
        public static unsafe void InitScriptHostNative(IntPtr hostApiPtr)
        {
            try
            {
                if (hostApiPtr == IntPtr.Zero)
                    return;
                Nodes.SetScriptHostApi((HostApi*)hostApiPtr);

                // A script-only project loads no plugin, so no OfsHost installs the PluginGuard fault
                // sinks. Install fallbacks routed through this (script) HostApi so a throwing script node
                // is still logged + notified instead of silently swallowed. `??=` so a loaded plugin's
                // sink (richer plugin-name attribution) keeps precedence when one exists.
                IntPtr apiPtr = hostApiPtr;
                PluginGuard.LogSink ??= (ctx, msg) => ScriptHostLog(apiPtr, ctx, msg);
                PluginGuard.FaultSink ??= ctx => ScriptHostReportFault(apiPtr, ctx);
            }
            catch
            {
                // Never let an exception cross back into native from this [UnmanagedCallersOnly] entry point.
            }
        }

        // Release a compiled script the host no longer references (its slot is nulled, its assembly unloaded;
        // see Nodes.ReleaseScript). Called on the main thread from ScriptSystem when a file's previous content
        // hash is superseded. `userData` is the slot the matching compile returned; `signal` is unused now
        // that functional and discrete script nodes share one slot table.
        [UnmanagedCallersOnly]
        public static void ReleaseScriptNative(int signal, IntPtr userData)
        {
            try
            {
                Nodes.ReleaseScript((int)(nint)userData);
            }
            catch
            {
                // Never let an exception cross back into native from this [UnmanagedCallersOnly] entry point.
            }
        }

        private static unsafe void ScriptHostLog(IntPtr hostApiPtr, string ctx, string msg)
        {
            var api = (HostApi*)hostApiPtr;
            if (api == null || api->HostLog == null)
                return;
            byte[] bytes = Encoding.UTF8.GetBytes($"[{ctx}] {msg}\0");
            fixed (byte* p = bytes)
                api->HostLog(api->Ctx, (int)LogLevel.Error, p);
        }

        private static unsafe void ScriptHostReportFault(IntPtr hostApiPtr, string faultCtx)
        {
            var api = (HostApi*)hostApiPtr;
            if (api == null || api->HostReportFault == null)
                return;
            byte[] bytes = Encoding.UTF8.GetBytes(faultCtx + "\0");
            fixed (byte* p = bytes)
                api->HostReportFault(api->Ctx, p);
        }

        // One user-declared param, parsed from the packed paramSpec (see compile_script_native_fn in
        // src/Services/ScriptSystem.h). Kind ('f'|'i'|'b'|'e') drives the named-local's type injected
        // before the user body: f→float, i/e→int (enum injects the 0-based index), b→bool.
        private readonly record struct ScriptParam(string Name, char Kind, float Default);

        // Compile a script body into the (signal, named-pin) method shape, bind it to a delegate,
        // and append it to the shared slot tables. On success fills *outResult and returns 0. On a
        // compile error writes UTF-8 diagnostics to errBuf and returns 1. On an unexpected failure
        // returns a negative code (and writes the exception text to errBuf).
        //
        // `inputNamesPtr`/`outputNamesPtr` are the header's named pins, one per line; the line count is
        // the pin count. Each becomes a named local injected before the body (an input read / an output
        // write target). `paramSpecPtr` is the packed user-param spec ("<name>\t<f|i>\t<default>\n" per
        // line, declaration order); each becomes a named local too. Null/empty = none.
        [UnmanagedCallersOnly]
        public static unsafe int CompileScriptNative(IntPtr sourcePtr, int signal, IntPtr inputNamesPtr,
                                                     IntPtr outputNamesPtr, IntPtr paramSpecPtr,
                                                     IntPtr outResultPtr, IntPtr errBufPtr, int errBufSize)
        {
            byte* errBuf = (byte*)errBufPtr;
            try
            {
                string? source = Marshal.PtrToStringUTF8(sourcePtr);
                if (source == null)
                {
                    WriteError(errBuf, errBufSize, "null source");
                    return -1;
                }

                var sig = (OfsSignalKind)signal;
                var inputNames = ParseNames(inputNamesPtr, 16);
                var outputNames = ParseNames(outputNamesPtr, 16);
                if (outputNames.Count == 0)
                    outputNames.Add("out"); // a node always has at least one output (the implicit "out")

                var scriptParams = ParseParamSpec(paramSpecPtr);
                string wrapper = BuildWrapper(sig, inputNames, outputNames, source, scriptParams);

                var tree = CSharpSyntaxTree.ParseText(wrapper, new CSharpParseOptions(LanguageVersion.Latest));
                var comp = CSharpCompilation.Create(
                    "OfsScript_" + Guid.NewGuid().ToString("N"),
                    new[] { tree },
                    GatherReferences(),
                    new CSharpCompilationOptions(OutputKind.DynamicallyLinkedLibrary,
                                                 optimizationLevel: OptimizationLevel.Release));

                using var ms = new MemoryStream();
                var emit = comp.Emit(ms);
                if (!emit.Success)
                {
                    var sb = new StringBuilder();
                    foreach (var d in emit.Diagnostics.Where(d => d.Severity == DiagnosticSeverity.Error).Take(20))
                        sb.AppendLine(d.ToString());
                    WriteError(errBuf, errBufSize, sb.ToString());
                    return 1;
                }

                ms.Seek(0, SeekOrigin.Begin);
                var alc = new ScriptLoadContext();
                var asm = alc.LoadFromStream(ms);
                var type = asm.GetType("OfsScript");
                var method = type?.GetMethod("Eval", BindingFlags.Public | BindingFlags.Static);
                if (method == null)
                {
                    WriteError(errBuf, errBufSize, "compiled assembly has no OfsScript.Eval");
                    return -2;
                }

                (IntPtr trampoline, IntPtr userData) appended;
                if (sig == OfsSignalKind.Functional)
                {
                    var del = method.CreateDelegate<ScriptFunctionalEval>();
                    appended = Nodes.AppendFunctional(del, alc);
                }
                else
                {
                    var del = method.CreateDelegate<ScriptDiscreteEval>();
                    appended = Nodes.AppendDiscrete(del, alc);
                }

                var result = (ScriptCompileResult*)outResultPtr;
                result->Fn = appended.trampoline;
                result->UserData = appended.userData;
                result->Signal = (int)sig;
                return 0;
            }
            catch (Exception ex)
            {
                WriteError(errBuf, errBufSize, ex.Message);
                return -3;
            }
        }

        // Wrap the user body into the array-shaped Eval method (one signature serves any shape). Each named
        // input pin is injected as a local bound to its slot (a `float`/`DiscreteReader`), each named output
        // pin as a write target; the raw `ins`/`outs` spans stay in scope for advanced/dynamic use. Each
        // declared param becomes a named local bound to ctx.Param(i, default). All injected locals come
        // BEFORE the "#line 1" directive so user error line numbers stay 1-based.
        //
        // Single-output functional keeps the `return <float>` idiom (an EvalScalar helper writes outs[0]).
        // Multi-output functional drops `return`: the body assigns the named output locals and the wrapper
        // copies each into its slot after the body runs. Discrete bodies write the named DiscreteWriters
        // (with `outp` aliasing outs[0] for the common single-output case).
        private static string BuildWrapper(OfsSignalKind signal, List<string> inputNames, List<string> outputNames,
                                           string body, List<ScriptParam> scriptParams)
        {
            var sb = new StringBuilder();
            sb.Append("using System;\n");
            sb.Append("using System.Collections.Generic;\n");
            sb.Append("using Ofs;\n");
            sb.Append("public static class OfsScript {\n");

            if (signal == OfsSignalKind.Functional)
            {
                if (outputNames.Count == 1)
                {
                    // A helper returning the float so single-output `return ...;` bodies compile unchanged;
                    // Eval writes its result into the single output pin.
                    sb.Append("    static float EvalScalar(double t, ReadOnlySpan<float> ins, NodeContext ctx) {\n");
                    AppendInputLocals(sb, inputNames, "float", "ins");
                    AppendParamLocals(sb, scriptParams);
                    sb.Append("#line 1 \"script\"\n");
                    sb.Append(body).Append('\n');
                    sb.Append("    }\n");
                    sb.Append("    public static void Eval(double t, ReadOnlySpan<float> ins, NodeContext ctx, "
                              + "Span<float> outs) {\n");
                    sb.Append("        outs[0] = EvalScalar(t, ins, ctx);\n");
                    sb.Append("    }\n");
                }
                else
                {
                    // Multi-output: the body assigns the named output locals; the wrapper flushes them to
                    // their slots. Each output starts at the neutral 50 so an unassigned pin is well-defined.
                    sb.Append("    public static void Eval(double t, ReadOnlySpan<float> ins, NodeContext ctx, "
                              + "Span<float> outs) {\n");
                    AppendInputLocals(sb, inputNames, "float", "ins");
                    for (int k = 0; k < outputNames.Count; k++)
                        if (IsInjectable(outputNames[k]))
                            sb.Append("        float ").Append(outputNames[k]).Append(" = 50f;\n");
                    AppendParamLocals(sb, scriptParams);
                    sb.Append("#line 1 \"script\"\n");
                    sb.Append(body).Append('\n');
                    sb.Append("#line default\n");
                    for (int k = 0; k < outputNames.Count; k++)
                        if (IsInjectable(outputNames[k]))
                            sb.Append("        outs[").Append(k).Append("] = ").Append(outputNames[k]).Append(";\n");
                    sb.Append("    }\n");
                }
            }
            else
            {
                sb.Append("    public static void Eval(ReadOnlySpan<DiscreteReader> ins, NodeContext ctx, "
                          + "ReadOnlySpan<DiscreteWriter> outs) {\n");
                AppendInputLocals(sb, inputNames, "DiscreteReader", "ins");
                for (int k = 0; k < outputNames.Count; k++)
                    if (IsInjectable(outputNames[k]))
                        sb.Append("        DiscreteWriter ").Append(outputNames[k]).Append(" = outs[").Append(k)
                          .Append("];\n");
                // `outp` aliases the first output for the common single-output discrete body, unless a pin is
                // already named "outp" (then the named writer above owns the name).
                if (!outputNames.Contains("outp"))
                    sb.Append("        DiscreteWriter outp = outs[0];\n");
                AppendParamLocals(sb, scriptParams);
                sb.Append("#line 1 \"script\"\n");
                sb.Append(body).Append('\n');
                sb.Append("    }\n");
            }

            sb.Append("}\n");
            return sb.ToString();
        }

        // Inject one named local per input pin, bound to ins[i] of the given element type. A name that
        // isn't injectable (not a valid identifier, or a C# keyword like the implicit "out") is skipped —
        // the raw span slot (ins[i]) is still in scope for it.
        private static void AppendInputLocals(StringBuilder sb, List<string> inputNames, string elemType, string span)
        {
            for (int i = 0; i < inputNames.Count; i++)
                if (IsInjectable(inputNames[i]))
                    sb.Append("        ").Append(elemType).Append(' ').Append(inputNames[i]).Append(" = ")
                      .Append(span).Append('[').Append(i).Append("];\n");
        }

        // A pin/param name is injectable as a named local only if it is a valid C# identifier AND not a
        // reserved keyword. The implicit single output is named "out" (a keyword), so it is never injected
        // as a local — single-output bodies use `return`/`outp`, and the raw outs[k] slot is always present.
        private static bool IsInjectable(string name) => IsIdentifier(name) && !s_csharpKeywords.Contains(name);

        private static readonly System.Collections.Generic.HashSet<string> s_csharpKeywords = new()
        {
            "abstract", "as", "base", "bool", "break", "byte", "case", "catch", "char", "checked", "class",
            "const", "continue", "decimal", "default", "delegate", "do", "double", "else", "enum", "event",
            "explicit", "extern", "false", "finally", "fixed", "float", "for", "foreach", "goto", "if",
            "implicit", "in", "int", "interface", "internal", "is", "lock", "long", "namespace", "new", "null",
            "object", "operator", "out", "override", "params", "private", "protected", "public", "readonly",
            "ref", "return", "sbyte", "sealed", "short", "sizeof", "stackalloc", "static", "string", "struct",
            "switch", "this", "throw", "true", "try", "typeof", "uint", "ulong", "unchecked", "unsafe", "ushort",
            "using", "virtual", "void", "volatile", "while",
        };

        // Inject one named local per declared param, bound to ctx.Param(i, default).
        private static void AppendParamLocals(StringBuilder sb, List<ScriptParam> scriptParams)
        {
            for (int i = 0; i < scriptParams.Count; i++)
            {
                var p = scriptParams[i];
                if (!IsIdentifier(p.Name)) // defense-in-depth; C++ already validated
                    continue;
                string lit = p.Default.ToString("R", CultureInfo.InvariantCulture) + "f";
                if (p.Kind == 'b')
                    sb.Append("        bool ").Append(p.Name).Append(" = ctx.Param(")
                      .Append(i).Append(", ").Append(lit).Append(") != 0f;\n");
                else if (p.Kind == 'i' || p.Kind == 'e') // enum injects its 0-based index as an int
                    sb.Append("        int ").Append(p.Name).Append(" = (int)System.Math.Round(ctx.Param(")
                      .Append(i).Append(", ").Append(lit).Append("));\n");
                else
                    sb.Append("        float ").Append(p.Name).Append(" = ctx.Param(")
                      .Append(i).Append(", ").Append(lit).Append(");\n");
            }
        }

        // Parse a newline-packed pin-name list (one C# identifier per line) into a list, capped at `max`.
        // Empty/whitespace lines are dropped; a non-identifier name is kept (the wrapper skips injecting a
        // local for it, leaving only the raw span slot).
        private static List<string> ParseNames(IntPtr namesPtr, int max)
        {
            var list = new List<string>();
            string? spec = namesPtr == IntPtr.Zero ? null : Marshal.PtrToStringUTF8(namesPtr);
            if (string.IsNullOrEmpty(spec))
                return list;
            foreach (var line in spec.Split('\n'))
            {
                var name = line.Trim();
                if (name.Length == 0)
                    continue;
                list.Add(name);
                if (list.Count >= max)
                    break;
            }
            return list;
        }

        // Parse the packed param spec into the injection list. Tolerant: malformed lines are skipped.
        private static unsafe List<ScriptParam> ParseParamSpec(IntPtr paramSpecPtr)
        {
            var list = new List<ScriptParam>();
            string? spec = paramSpecPtr == IntPtr.Zero ? null : Marshal.PtrToStringUTF8(paramSpecPtr);
            if (string.IsNullOrEmpty(spec))
                return list;
            foreach (var line in spec.Split('\n'))
            {
                if (line.Length == 0)
                    continue;
                var parts = line.Split('\t');
                if (parts.Length < 3 || !IsIdentifier(parts[0]))
                    continue;
                float def = float.TryParse(parts[2], NumberStyles.Float, CultureInfo.InvariantCulture, out var d) ? d : 0f;
                char kind = parts[1].Length > 0 ? parts[1][0] : 'f';
                list.Add(new ScriptParam(parts[0], kind, def));
            }
            return list;
        }

        private static bool IsIdentifier(string s)
        {
            if (s.Length == 0 || !(char.IsLetter(s[0]) || s[0] == '_'))
                return false;
            foreach (char c in s)
                if (!(char.IsLetterOrDigit(c) || c == '_'))
                    return false;
            return true;
        }

        private static MetadataReference[]? s_baseRefs;
        private static readonly object s_baseRefsLock = new();

        private static MetadataReference[] GatherReferences()
        {
            // CompileScriptNative runs on JobSystem worker threads, so concurrent first-compiles race here.
            // Without the lock two builders each read the full framework metadata (~172 MetadataReference
            // images) but only one wins the s_baseRefs assignment; the loser's images are never referenced
            // again yet stay alive in the CLR, leaking the whole reference set. Lock the build + publish so
            // it happens exactly once. The cached path holds the lock only for a field read.
            lock (s_baseRefsLock)
            {
                if (s_baseRefs != null)
                    return s_baseRefs;

                var refs = new System.Collections.Generic.List<MetadataReference>();
                bool haveApi = false;
                var tpa = AppContext.GetData("TRUSTED_PLATFORM_ASSEMBLIES") as string;
                if (!string.IsNullOrEmpty(tpa))
                {
                    foreach (var path in tpa.Split(Path.PathSeparator))
                    {
                        if (path.Length == 0)
                            continue;
                        refs.Add(MetadataReference.CreateFromFile(path));
                        if (string.Equals(Path.GetFileNameWithoutExtension(path), "Ofs.Api",
                                          StringComparison.OrdinalIgnoreCase))
                            haveApi = true;
                    }
                }
                // Ofs.Api is loaded dynamically, so it is usually absent from the TPA list — add it.
                if (!haveApi)
                {
                    var apiLoc = typeof(NodeContext).Assembly.Location;
                    if (!string.IsNullOrEmpty(apiLoc))
                        refs.Add(MetadataReference.CreateFromFile(apiLoc));
                }

                s_baseRefs = refs.ToArray();
                return s_baseRefs;
            }
        }

        private static unsafe void WriteError(byte* buf, int size, string msg)
        {
            if (buf == null || size <= 0)
                return;
            var bytes = Encoding.UTF8.GetBytes(msg);
            int n = Math.Min(bytes.Length, size - 1);
            for (int i = 0; i < n; i++)
                buf[i] = bytes[i];
            buf[n] = 0;
        }
    }
}
