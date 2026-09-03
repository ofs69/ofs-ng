#pragma once

#include "Core/StandardAxis.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace ofs::ws {

inline constexpr std::string_view kPath = "/ofs";
inline constexpr std::string_view kSubprotocol = "ofs-api.json";

struct SeekCommand {
    double time = 0.0;
};
struct SetPlayingCommand {
    bool playing = false;
};
struct SetSpeedCommand {
    float speed = 1.0f;
};
using Command = std::variant<SeekCommand, SetPlayingCommand, SetSpeedCommand>;

// Parse the classic OFS command envelope. Unknown or malformed commands are ignored.
std::optional<Command> parseCommand(std::string_view text);

// Build the RFC 6455 upgrade response for GET /ofs. Returns nullopt for an invalid request.
std::optional<std::string> handshakeResponse(std::string_view request);

// The name a script is published under: `baseName` (the media/project file name, extension already
// stripped) for the primary stroke axis, `baseName.<axis-tag>` for every other axis. Classic OFS took
// this from Funscript::Title() — a title, not a file name — and clients key their axis mapping off the
// last dot-separated segment, so the ".funscript" extension must stay off: OFS_Simulator3D reads it as
// the axis tag and then recognizes no axis at all, and MultiFunPlayer appends the extension itself.
std::string scriptName(std::string_view baseName, StandardAxis role);

// Encode one unmasked server-to-client frame.
std::string encodeFrame(uint8_t opcode, std::string_view payload);
void encodeFrame(std::string &frame, uint8_t opcode, std::string_view payload);

// Extract one complete client-to-server frame from buffer. Client frames must be masked.
// Returns nullopt when more bytes are required; protocolError is set for malformed input.
struct Frame {
    uint8_t opcode = 0;
    bool final = true;
    std::string payload;
};
std::optional<Frame> consumeFrame(std::vector<uint8_t> &buffer, bool &protocolError);

} // namespace ofs::ws
