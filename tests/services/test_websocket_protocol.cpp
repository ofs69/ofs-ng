#include "Services/WebSocketProtocol.h"

#include <doctest/doctest.h>
#include <string>
#include <variant>
#include <vector>

using namespace ofs::ws;

TEST_CASE("WebSocket handshake matches the RFC 6455 accept example and OFS subprotocol") {
    const std::string request = "GET /ofs HTTP/1.1\r\n"
                                "Host: 127.0.0.1:8080\r\n"
                                "Upgrade: websocket\r\n"
                                "Connection: keep-alive, Upgrade\r\n"
                                "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                                "Sec-WebSocket-Version: 13\r\n"
                                "Sec-WebSocket-Protocol: other, ofs-api.json\r\n\r\n";

    const auto response = handshakeResponse(request);
    REQUIRE(response.has_value());
    CHECK(response->find("101 Switching Protocols") != std::string::npos);
    CHECK(response->find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != std::string::npos);
    CHECK(response->find("Sec-WebSocket-Protocol: ofs-api.json") != std::string::npos);
}

TEST_CASE("WebSocket handshake rejects paths other than the classic OFS endpoint") {
    const std::string request = "GET /wrong HTTP/1.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                                "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
    CHECK_FALSE(handshakeResponse(request).has_value());
}

TEST_CASE("WebSocket handshake accepts clients that do not request a subprotocol") {
    const std::string request =
        "GET /ofs HTTP/1.1\r\nHost: 127.0.0.1:8080\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";
    const auto response = handshakeResponse(request);
    REQUIRE(response.has_value());
    CHECK(response->find("101 Switching Protocols") != std::string::npos);
    CHECK(response->find("Sec-WebSocket-Protocol") == std::string::npos);
}

TEST_CASE("Classic OFS commands parse into typed intents") {
    const auto seek = parseCommand(R"({"type":"command","name":"change_time","data":{"time":12.5}})");
    REQUIRE(seek.has_value());
    REQUIRE(std::holds_alternative<SeekCommand>(*seek));
    CHECK(std::get<SeekCommand>(*seek).time == doctest::Approx(12.5));

    const auto play = parseCommand(R"({"type":"command","name":"change_play","data":{"playing":true}})");
    REQUIRE(play.has_value());
    REQUIRE(std::holds_alternative<SetPlayingCommand>(*play));
    CHECK(std::get<SetPlayingCommand>(*play).playing);

    const auto speed = parseCommand(R"({"type":"command","name":"change_playbackspeed","data":{"speed":1.25}})");
    REQUIRE(speed.has_value());
    REQUIRE(std::holds_alternative<SetSpeedCommand>(*speed));
    CHECK(std::get<SetSpeedCommand>(*speed).speed == doctest::Approx(1.25f));

    CHECK_FALSE(parseCommand(R"({"type":"event","name":"change_time","data":{"time":1}})").has_value());
    CHECK_FALSE(parseCommand(R"({"type":"command","name":"unknown","data":{}})").has_value());
}

TEST_CASE("Malformed Classic OFS command fields are ignored without throwing") {
    const std::string_view malformed[] = {
        R"({"type":1,"name":"change_time","data":{"time":1}})",
        R"({"type":"command","name":false,"data":{"time":1}})",
        R"({"type":"command","name":"change_time","data":{"time":"soon"}})",
        R"({"type":"command","name":"change_play","data":{"playing":1}})",
        R"({"type":"command","name":"change_playbackspeed","data":{"speed":1e100}})",
    };
    for (const std::string_view text : malformed) {
        CHECK_NOTHROW(parseCommand(text));
        CHECK_FALSE(parseCommand(text).has_value());
    }
}

TEST_CASE("Masked client frames are decoded and consumed") {
    const std::string payload = "hello";
    const unsigned char mask[4] = {0x37, 0xfa, 0x21, 0x3d};
    std::vector<uint8_t> bytes = {0x81,   static_cast<uint8_t>(0x80u | payload.size()), mask[0], mask[1], mask[2],
                                  mask[3]};
    for (size_t i = 0; i < payload.size(); ++i)
        bytes.push_back(static_cast<uint8_t>(payload[i]) ^ mask[i % 4]);

    bool protocolError = false;
    const auto frame = consumeFrame(bytes, protocolError);
    REQUIRE(frame.has_value());
    CHECK_FALSE(protocolError);
    CHECK(frame->opcode == 0x1);
    CHECK(frame->final);
    CHECK(frame->payload == payload);
    CHECK(bytes.empty());
}

TEST_CASE("Server frames use the unmasked RFC 6455 length encodings") {
    CHECK(encodeFrame(0x1, "ok") == std::string("\x81\x02ok", 4));
    const std::string payload(126, 'x');
    const std::string frame = encodeFrame(0x1, payload);
    REQUIRE(frame.size() == payload.size() + 4);
    CHECK(static_cast<unsigned char>(frame[1]) == 126);
    CHECK(static_cast<unsigned char>(frame[2]) == 0);
    CHECK(static_cast<unsigned char>(frame[3]) == 126);
}

TEST_CASE("Server frame encoding can reuse caller-owned storage") {
    std::string frame;
    frame.reserve(256);
    encodeFrame(frame, 0x1, "first");
    CHECK(frame == std::string("\x81\x05"
                               "first",
                               7));

    encodeFrame(frame, 0xA, "ok");
    CHECK(frame == std::string("\x8a\x02ok", 4));
}

TEST_CASE("Script names are Classic OFS titles: extension-free, axis tag last") {
    // Third-party clients (OFS_Simulator3D, MultiFunPlayer) map a script to an axis by the last
    // dot-separated segment of this name, so a trailing ".funscript" would hide the tag from all of them.
    CHECK(scriptName("video", ofs::StandardAxis::L0) == "video");
    CHECK(scriptName("video", ofs::StandardAxis::L1) == "video.L1");
    CHECK(scriptName("video", ofs::StandardAxis::R1) == "video.R1");
    CHECK(scriptName("my.video.2160p", ofs::StandardAxis::R2) == "my.video.2160p.R2");
}
