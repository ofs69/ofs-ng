using System;
using System.IO;
using System.Net.Http;
using System.Runtime.InteropServices;

namespace Ofs.HostServices
{
    // A blocking HTTPS GET exposed to the native host. The host calls HttpGet from a JobSystem worker
    // thread (CoreCLR attaches the calling thread on the reverse-pinvoke transition), so the synchronous
    // HttpClient.Send is fine here. TLS chains are validated against the OS trust store by the runtime —
    // no CA bundle ships with the app. The response body is handed back through a native callback rather
    // than a returned buffer so no allocation crosses the boundary to be freed (which allocator freed it
    // is otherwise ambiguous across the managed/native seam).
    public static class HttpService
    {
        // One shared client (the documented pattern: a new HttpClient per request leaks sockets). The
        // SocketsHttpHandler is platform-portable and uses the OS certificate store on every target.
        private static readonly HttpClient _client = CreateClient();

        private static HttpClient CreateClient()
        {
            var handler = new SocketsHttpHandler
            {
                ConnectTimeout = TimeSpan.FromSeconds(10),
                AllowAutoRedirect = true,
                MaxAutomaticRedirections = 5,
            };
            return new HttpClient(handler) { Timeout = TimeSpan.FromSeconds(20) };
        }

        // url / userAgent / headers are UTF-8 char* from native (headers is a '\n'-separated block of
        // "Name: value" lines, or null). ctx + sink are an opaque native pointer and a
        // void(ctx, status, bodyPtr, bodyLen) callback the host fills its HttpResponse from. Returns 0 when
        // the request COMPLETED (the sink was called with a valid status, even a non-2xx one) and -1 on any
        // transport-level failure (DNS, TLS, timeout) — matching the native httpGet contract. Never throws
        // across the boundary.
        [UnmanagedCallersOnly]
        public static unsafe int HttpGet(IntPtr url, IntPtr userAgent, IntPtr headers, IntPtr ctx, IntPtr sink)
        {
            try
            {
                string? target = Marshal.PtrToStringUTF8(url);
                if (string.IsNullOrEmpty(target))
                    return -1;

                using var req = new HttpRequestMessage(HttpMethod.Get, target);

                string? ua = Marshal.PtrToStringUTF8(userAgent);
                if (!string.IsNullOrEmpty(ua))
                    req.Headers.TryAddWithoutValidation("User-Agent", ua);

                string? headerBlock = Marshal.PtrToStringUTF8(headers);
                if (!string.IsNullOrEmpty(headerBlock))
                {
                    foreach (string line in headerBlock.Split('\n', StringSplitOptions.RemoveEmptyEntries))
                    {
                        int colon = line.IndexOf(':');
                        if (colon <= 0)
                            continue;
                        string name = line[..colon].Trim();
                        string value = line[(colon + 1)..].Trim();
                        if (name.Length != 0)
                            req.Headers.TryAddWithoutValidation(name, value);
                    }
                }

                using HttpResponseMessage resp = _client.Send(req);
                byte[] body = ReadBody(resp);

                var emit = (delegate* unmanaged<IntPtr, int, IntPtr, int, void>)sink;
                fixed (byte* p = body)
                {
                    emit(ctx, (int)resp.StatusCode, (IntPtr)p, body.Length);
                }
                return 0;
            }
            catch
            {
                return -1; // never let an exception cross back into native
            }
        }

        private static byte[] ReadBody(HttpResponseMessage resp)
        {
            using Stream s = resp.Content.ReadAsStream();
            using var ms = new MemoryStream();
            s.CopyTo(ms);
            return ms.ToArray();
        }
    }
}
