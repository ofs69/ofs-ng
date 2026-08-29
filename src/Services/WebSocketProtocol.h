#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ofs::ws {

inline constexpr std::string_view kPath = "/ofs";
inline constexpr std::string_view kSubprotocol = "ofs-api.json";

enum class CommandKind { Seek, SetPlaying, SetSpeed };

struct Command {
    CommandKind kind = CommandKind::Seek;
    double number = 0.0;
    bool boolean = false;
};

// Parse the classic OFS command envelope. Unknown or malformed commands are ignored.
std::optional<Command> parseCommand(std::string_view text);

// Build the RFC 6455 upgrade response for GET /ofs. Returns nullopt for an invalid request.
std::optional<std::string> handshakeResponse(std::string_view request);

// Encode one unmasked server-to-client frame.
std::string encodeFrame(uint8_t opcode, std::string_view payload);

// Extract one complete client-to-server frame from buffer. Client frames must be masked.
// Returns nullopt when more bytes are required; protocolError is set for malformed input.
struct Frame {
    uint8_t opcode = 0;
    bool final = true;
    std::string payload;
};
std::optional<Frame> consumeFrame(std::vector<uint8_t> &buffer, bool &protocolError);

} // namespace ofs::ws
