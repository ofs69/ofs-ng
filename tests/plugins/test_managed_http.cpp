// Exercises the full native↔managed HTTP seam (Ofs.HostServices): initManagedHttp() loads the assembly,
// resolves the entry point, and registers the backend; util::httpGet then marshals a request across to
// .NET's HttpClient and the response back through the native callback sink. Hermetic — it serves a canned
// response from a one-shot loopback TCP server on 127.0.0.1 rather than touching the real network, so it
// is deterministic and independent of whether the GitHub repo is reachable/public.

#include <doctest/doctest.h>

#include "Services/ManagedHttp.h"
#include "Util/Http.h"

#include <string>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using sock_t = SOCKET;
#define OFS_CLOSESOCK closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using sock_t = int;
constexpr sock_t INVALID_SOCKET = -1;
#define OFS_CLOSESOCK ::close
#endif

namespace {

// Bind a loopback listener on an ephemeral port and return it (with the chosen port). On Windows the
// caller balances the WSAStartup here with a WSACleanup after closing the socket.
sock_t startLoopbackListener(unsigned short &outPort) {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    sock_t s = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0; // ephemeral — the OS assigns a free port
    bind(s, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
    listen(s, 1);

    socklen_t len = sizeof(addr);
    getsockname(s, reinterpret_cast<sockaddr *>(&addr), &len);
    outPort = ntohs(addr.sin_port);
    return s;
}

// Accept one connection, drain the request bytes, send the canned response, and close.
void serveOnce(sock_t listener, const std::string &response) {
    sock_t c = accept(listener, nullptr, nullptr);
    if (c == INVALID_SOCKET)
        return;
    char scratch[2048];
    recv(c, scratch, sizeof(scratch), 0); // read (and ignore) the request line/headers
    send(c, response.data(), static_cast<int>(response.size()), 0);
    OFS_CLOSESOCK(c);
}

} // namespace

TEST_CASE("managed http backend round-trips a loopback response") {
    REQUIRE(ofs::initManagedHttp());

    unsigned short port = 0;
    sock_t listener = startLoopbackListener(port);
    REQUIRE(port != 0);

    const std::string body = R"({"tag_name":"v9.9.9","html_url":"https://example/r"})";
    const std::string response =
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) +
        "\r\nConnection: close\r\n\r\n" + body;

    std::thread server([&] { serveOnce(listener, response); });

    auto got = ofs::util::httpGet("http://127.0.0.1:" + std::to_string(port) + "/", "ofs-ng-test",
                                  {"Accept: application/json"});

    server.join();
    OFS_CLOSESOCK(listener);
#ifdef _WIN32
    WSACleanup();
#endif

    REQUIRE(got.has_value()); // the request completed end-to-end through the managed backend
    CHECK(got->status == 200);
    CHECK(got->body == body); // body survived the UTF-8 marshal back through the native sink intact
}
