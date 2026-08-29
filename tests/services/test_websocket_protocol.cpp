#include "Services/WebSocketProtocol.h"

#include <doctest/doctest.h>
#include <string>
#include <vector>

using namespace ofs::ws;

TEST_CASE("WebSocket handshake matches the RFC 6455 accept example and OFS subprotocol") {
    const std::string request =
        "GET /ofs HTTP/1.1\r\n"
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
    const std::string request =
        "GET /wrong HTTP/1.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
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
    CHECK(seek->kind == CommandKind::Seek);
    CHECK(seek->number == doctest::Approx(12.5));

    const auto play = parseCommand(R"({"type":"command","name":"change_play","data":{"playing":true}})");
    REQUIRE(play.has_value());
    CHECK(play->kind == CommandKind::SetPlaying);
    CHECK(play->boolean);

    const auto speed =
        parseCommand(R"({"type":"command","name":"change_playbackspeed","data":{"speed":1.25}})");
    REQUIRE(speed.has_value());
    CHECK(speed->kind == CommandKind::SetSpeed);
    CHECK(speed->number == doctest::Approx(1.25));

    CHECK_FALSE(parseCommand(R"({"type":"event","name":"change_time","data":{"time":1}})").has_value());
    CHECK_FALSE(parseCommand(R"({"type":"command","name":"unknown","data":{}})").has_value());
}

TEST_CASE("Masked client frames are decoded and consumed") {
    const std::string payload = "hello";
    const unsigned char mask[4] = {0x37, 0xfa, 0x21, 0x3d};
    std::vector<uint8_t> bytes = {0x81, static_cast<uint8_t>(0x80u | payload.size()), mask[0], mask[1], mask[2],
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
