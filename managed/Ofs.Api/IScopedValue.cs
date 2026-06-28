using System;

namespace Ofs
{
    // The host roots every live AppScoped<T> / ProjectScoped<T> through this seam: it flushes each one's
    // pending edit once per frame (Sync) and disposes it (its reused JSON writer) when the plugin unloads.
    // Internal — the two scoped wrappers are the only implementers and OfsHost the only consumer.
    internal interface IScopedValue : IDisposable
    {
        void Sync();
    }
}
