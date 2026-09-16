#include "Core/AxisEvents.h"
#include "Format/AppSettings.h"
#include "Services/WebSocketApi.h"
#include "helpers/TestProject.h"

#include <array>
#include <chrono>
#include <doctest/doctest.h>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <WS2tcpip.h>
#include <WinSock2.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

// A real loopback client: assert on the same snapshot and updates a player receives.
struct Client {
    Client() = default;
    Client(const Client &) = delete;
    Client &operator=(const Client &) = delete;
#ifdef _WIN32
    SOCKET socket = INVALID_SOCKET;
    ~Client() { closesocket(socket); }
#else
    int socket = -1;
    ~Client() { close(socket); }
#endif
    std::string input;
    bool upgraded = false;

    void connectTo(int port) {
        socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<uint16_t>(port));
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        REQUIRE(connect(socket, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
        constexpr std::string_view request =
            "GET /ofs HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";
        REQUIRE(send(socket, request.data(), static_cast<int>(request.size()), 0) == static_cast<int>(request.size()));
#ifdef _WIN32
        u_long enabled = 1;
        REQUIRE(ioctlsocket(socket, FIONBIO, &enabled) == 0);
#else
        REQUIRE(fcntl(socket, F_SETFL, O_NONBLOCK) == 0);
#endif
    }

    std::vector<nlohmann::json> receive(ofs::WebSocketApi &api) {
        std::vector<nlohmann::json> events;
        // Give TCP delayed delivery a bounded window, including when no event is expected.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
        do {
            api.update(0.01f);
            std::array<char, 8192> bytes{};
            const auto count = recv(socket, bytes.data(), static_cast<int>(bytes.size()), 0);
            if (count > 0)
                input.append(bytes.data(), static_cast<size_t>(count));
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < deadline);
        if (!upgraded) {
            const auto end = input.find("\r\n\r\n");
            REQUIRE(end != std::string::npos);
            REQUIRE(input.starts_with("HTTP/1.1 101"));
            input.erase(0, end + 4);
            upgraded = true;
        }
        while (input.size() >= 2) {
            const auto first = static_cast<uint8_t>(input[0]);
            const auto second = static_cast<uint8_t>(input[1]);
            REQUIRE(first == 0x81);
            REQUIRE((second & 0x80u) == 0);
            size_t length = second;
            size_t header = 2;
            if (length == 126 || length == 127) {
                header += length == 126 ? 2 : 8;
                if (input.size() < header)
                    break;
                length = 0;
                for (size_t i = 2; i < header; ++i)
                    length = (length << 8u) | static_cast<uint8_t>(input[i]);
            }
            if (input.size() - header < length)
                break;
            events.push_back(nlohmann::json::parse(input.substr(header, length)));
            input.erase(0, header + length);
        }
        return events;
    }
};

void checkScripts(const std::vector<nlohmann::json> &events, const std::vector<std::string> &expected) {
    std::vector<std::string> names;
    for (const auto &event : events) {
        if (event.value("name", "") == "funscript_change") {
            const auto &data = event.at("data");
            names.push_back(data.at("name").get<std::string>());
            if (data.at("name") == "video") {
                REQUIRE(data.at("funscript").at("actions").size() == 1);
                CHECK(data.at("funscript").at("actions")[0].at("pos") == 25);
            }
        }
        CHECK(event.value("name", "") != "funscript_remove");
    }
    CHECK(names == expected);
}

} // namespace

TEST_CASE("WebSocket scratch axes never replace L0 on connect or update") {
    ofs::test::TestProject fixture;
    auto &project = fixture.project;
    project.state.mediaPath = "video.mp4";
    project.axes[0].actions.insert({1.0, 25});
    project.axes[static_cast<size_t>(ofs::StandardAxis::R1)].actions.insert({1.0, 50});
    for (auto i = static_cast<size_t>(ofs::StandardAxis::S0); i < ofs::kStandardAxisCount; ++i) {
        project.axes[i].showInStrip = true;
        project.axes[i].actions.insert({1.0, 90});
    }
    ofs::AppSettings settings;
    settings.webSocketServerEnabled = true;
    ofs::WebSocketApi api(project, fixture.eq, settings);
    // Try a bounded range so an unrelated local listener does not make the test flaky.
    for (int port = 49152; port < 49252 && !api.status().running; ++port) {
        settings.webSocketPort = port;
        api.update(0.0f);
    }
    REQUIRE(api.status().running);
    Client client;
    client.connectTo(settings.webSocketPort);
    checkScripts(client.receive(api), {"video", "video.R1"});

    SUBCASE("scratch edits and evaluation results do not publish fallback stroke scripts") {
        fixture.eq.push(ofs::AxisModifiedEvent{ofs::StandardAxis::S0});
        fixture.eq.push(ofs::AxisModifiedEvent{ofs::StandardAxis::S9});
        fixture.eq.push(ofs::EvalCompleteEvent{.role = ofs::StandardAxis::S9});
        fixture.eq.drain();
        api.update(0.25f);
        checkScripts(client.receive(api), {});
        fixture.eq.push(ofs::AxisModifiedEvent{ofs::StandardAxis::L0});
        fixture.eq.drain();
        api.update(0.25f);
        checkScripts(client.receive(api), {"video"});
    }
    SUBCASE("scratch removal and recreation resynchronize only device axes") {
        const auto index = static_cast<size_t>(ofs::StandardAxis::S9);
        project.axes[index].showInStrip = false;
        project.axes[index].actions.clear();
        checkScripts(client.receive(api), {"video", "video.R1"});
        project.axes[index].showInStrip = true;
        checkScripts(client.receive(api), {"video", "video.R1"});
    }
    SUBCASE("another connection receives the same device axes") {
        Client second;
        second.connectTo(settings.webSocketPort);
        checkScripts(second.receive(api), {"video", "video.R1"});
    }
    SUBCASE("hidden scratch data remains private on resynchronization") {
        for (auto i = static_cast<size_t>(ofs::StandardAxis::S0); i < ofs::kStandardAxisCount; ++i)
            project.axes[i].showInStrip = false;
        checkScripts(client.receive(api), {"video", "video.R1"});
    }
}
