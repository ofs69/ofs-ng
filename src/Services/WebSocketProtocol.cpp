#include "Services/WebSocketProtocol.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>
#include <nlohmann/json.hpp>
#include <ranges>

namespace ofs::ws {
namespace {

constexpr size_t kMaxPayload = 16u * 1024u * 1024u;

std::string_view trim(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
        value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r'))
        value.remove_suffix(1);
    return value;
}

bool iequals(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::ranges::equal(a, b, [](char lhs, char rhs) {
               return std::tolower(static_cast<unsigned char>(lhs)) == std::tolower(static_cast<unsigned char>(rhs));
           });
}

bool containsToken(std::string_view value, std::string_view token) {
    while (!value.empty()) {
        const size_t comma = value.find(',');
        if (iequals(trim(value.substr(0, comma)), token))
            return true;
        if (comma == std::string_view::npos)
            return false;
        value.remove_prefix(comma + 1);
    }
    return false;
}

uint32_t rotateLeft(uint32_t value, int count) {
    return std::rotl(value, count);
}

std::array<uint8_t, 20> sha1(std::string_view input) {
    std::vector<uint8_t> data(input.begin(), input.end());
    const uint64_t bitLength = static_cast<uint64_t>(data.size()) * 8u;
    data.push_back(0x80);
    while ((data.size() % 64u) != 56u)
        data.push_back(0);
    for (int shift = 56; shift >= 0; shift -= 8)
        data.push_back(static_cast<uint8_t>((bitLength >> shift) & 0xffu));

    uint32_t h0 = 0x67452301u;
    uint32_t h1 = 0xefcdab89u;
    uint32_t h2 = 0x98badcfeu;
    uint32_t h3 = 0x10325476u;
    uint32_t h4 = 0xc3d2e1f0u;

    for (size_t chunk = 0; chunk < data.size(); chunk += 64) {
        std::array<uint32_t, 80> words{};
        for (size_t i = 0; i < 16; ++i) {
            const size_t p = chunk + i * 4;
            words[i] = (static_cast<uint32_t>(data[p]) << 24u) | (static_cast<uint32_t>(data[p + 1]) << 16u) |
                       (static_cast<uint32_t>(data[p + 2]) << 8u) | static_cast<uint32_t>(data[p + 3]);
        }
        for (size_t i = 16; i < words.size(); ++i)
            words[i] = rotateLeft(words[i - 3] ^ words[i - 8] ^ words[i - 14] ^ words[i - 16], 1);

        uint32_t a = h0;
        uint32_t b = h1;
        uint32_t c = h2;
        uint32_t d = h3;
        uint32_t e = h4;
        for (size_t i = 0; i < words.size(); ++i) {
            uint32_t f = 0;
            uint32_t k = 0;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5a827999u;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ed9eba1u;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8f1bbcdcu;
            } else {
                f = b ^ c ^ d;
                k = 0xca62c1d6u;
            }
            const uint32_t temp = rotateLeft(a, 5) + f + e + k + words[i];
            e = d;
            d = c;
            c = rotateLeft(b, 30);
            b = a;
            a = temp;
        }
        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
    }

    std::array<uint8_t, 20> result{};
    const std::array<uint32_t, 5> hash = {h0, h1, h2, h3, h4};
    for (size_t i = 0; i < hash.size(); ++i)
        for (size_t j = 0; j < 4; ++j)
            result[i * 4 + j] = static_cast<uint8_t>((hash[i] >> (24u - static_cast<unsigned>(j) * 8u)) & 0xffu);
    return result;
}

std::string base64(const std::array<uint8_t, 20> &bytes) {
    constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(28);
    for (size_t i = 0; i < bytes.size(); i += 3) {
        const uint32_t a = bytes[i];
        const uint32_t b = i + 1 < bytes.size() ? bytes[i + 1] : 0;
        const uint32_t c = i + 2 < bytes.size() ? bytes[i + 2] : 0;
        const uint32_t value = (a << 16u) | (b << 8u) | c;
        out.push_back(alphabet[(value >> 18u) & 63u]);
        out.push_back(alphabet[(value >> 12u) & 63u]);
        out.push_back(i + 1 < bytes.size() ? alphabet[(value >> 6u) & 63u] : '=');
        out.push_back(i + 2 < bytes.size() ? alphabet[value & 63u] : '=');
    }
    return out;
}

} // namespace

std::optional<Command> parseCommand(std::string_view text) {
    const nlohmann::json root = nlohmann::json::parse(text, nullptr, false);
    if (!root.is_object())
        return std::nullopt;
    const auto type = root.find("type");
    const auto name = root.find("name");
    if (type == root.end() || !type->is_string() || type->get_ref<const std::string &>() != "command" ||
        name == root.end() || !name->is_string())
        return std::nullopt;
    const auto data = root.find("data");
    if (data == root.end() || !data->is_object())
        return std::nullopt;

    const auto &commandName = name->get_ref<const std::string &>();
    if (commandName == "change_time") {
        const auto value = data->find("time");
        if (value != data->end() && value->is_number()) {
            const double time = value->get<double>();
            if (std::isfinite(time) && time >= 0.0)
                return SeekCommand{time};
        }
    } else if (commandName == "change_play") {
        const auto value = data->find("playing");
        if (value != data->end() && value->is_boolean())
            return SetPlayingCommand{value->get<bool>()};
    } else if (commandName == "change_playbackspeed") {
        const auto value = data->find("speed");
        if (value != data->end() && value->is_number()) {
            const double speed = value->get<double>();
            if (std::isfinite(speed) && speed > 0.0 &&
                speed <= static_cast<double>((std::numeric_limits<float>::max)()))
                return SetSpeedCommand{static_cast<float>(speed)};
        }
    }
    return std::nullopt;
}

std::optional<std::string> handshakeResponse(std::string_view request) {
    const size_t firstEnd = request.find("\r\n");
    if (firstEnd == std::string_view::npos || request.substr(0, firstEnd) != "GET /ofs HTTP/1.1")
        return std::nullopt;

    std::string_view key;
    bool upgrade = false;
    bool connectionUpgrade = false;
    bool wantsProtocol = false;
    for (size_t pos = firstEnd + 2; pos < request.size();) {
        const size_t end = request.find("\r\n", pos);
        if (end == std::string_view::npos || end == pos)
            break;
        const std::string_view line = request.substr(pos, end - pos);
        const size_t colon = line.find(':');
        if (colon != std::string_view::npos) {
            const std::string_view name = trim(line.substr(0, colon));
            const std::string_view value = trim(line.substr(colon + 1));
            if (iequals(name, "Sec-WebSocket-Key"))
                key = value;
            else if (iequals(name, "Upgrade"))
                upgrade = iequals(value, "websocket");
            else if (iequals(name, "Connection"))
                connectionUpgrade = containsToken(value, "upgrade");
            else if (iequals(name, "Sec-WebSocket-Protocol"))
                wantsProtocol = containsToken(value, kSubprotocol);
        }
        pos = end + 2;
    }
    if (key.empty() || !upgrade || !connectionUpgrade)
        return std::nullopt;

    std::string source(key);
    source += "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::string response = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n";
    response += "Sec-WebSocket-Accept: " + base64(sha1(source)) + "\r\n";
    if (wantsProtocol)
        response += "Sec-WebSocket-Protocol: ofs-api.json\r\n";
    response += "\r\n";
    return response;
}

std::string encodeFrame(uint8_t opcode, std::string_view payload) {
    std::string frame;
    encodeFrame(frame, opcode, payload);
    return frame;
}

void encodeFrame(std::string &frame, uint8_t opcode, std::string_view payload) {
    frame.clear();
    frame.reserve(payload.size() + 10);
    frame.push_back(static_cast<char>(0x80u | (opcode & 0x0fu)));
    if (payload.size() <= 125) {
        frame.push_back(static_cast<char>(payload.size()));
    } else if (payload.size() <= std::numeric_limits<uint16_t>::max()) {
        frame.push_back(126);
        frame.push_back(static_cast<char>((payload.size() >> 8u) & 0xffu));
        frame.push_back(static_cast<char>(payload.size() & 0xffu));
    } else {
        frame.push_back(127);
        const uint64_t size = payload.size();
        for (int shift = 56; shift >= 0; shift -= 8)
            frame.push_back(static_cast<char>((size >> shift) & 0xffu));
    }
    frame.append(payload);
}

std::optional<Frame> consumeFrame(std::vector<uint8_t> &buffer, bool &protocolError) {
    protocolError = false;
    if (buffer.size() < 2)
        return std::nullopt;
    const uint8_t first = buffer[0];
    const uint8_t second = buffer[1];
    if ((first & 0x70u) != 0 || (second & 0x80u) == 0) {
        protocolError = true;
        return std::nullopt;
    }

    size_t header = 2;
    uint64_t length = second & 0x7fu;
    if (length == 126) {
        if (buffer.size() < 4)
            return std::nullopt;
        length = (static_cast<uint64_t>(buffer[2]) << 8u) | buffer[3];
        header = 4;
    } else if (length == 127) {
        if (buffer.size() < 10)
            return std::nullopt;
        length = 0;
        for (size_t i = 2; i < 10; ++i)
            length = (length << 8u) | buffer[i];
        header = 10;
    }
    if (length > kMaxPayload || ((first & 0x08u) != 0 && (length > 125 || (first & 0x80u) == 0))) {
        protocolError = true;
        return std::nullopt;
    }
    if (buffer.size() < header + 4 || length > buffer.size() - header - 4)
        return std::nullopt;

    const std::array<uint8_t, 4> mask = {buffer[header], buffer[header + 1], buffer[header + 2], buffer[header + 3]};
    header += 4;
    Frame frame{.opcode = static_cast<uint8_t>(first & 0x0fu), .final = (first & 0x80u) != 0};
    frame.payload.resize(static_cast<size_t>(length));
    for (size_t i = 0; i < frame.payload.size(); ++i)
        frame.payload[i] = static_cast<char>(buffer[header + i] ^ mask[i % mask.size()]);
    buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(header + length));
    return frame;
}

} // namespace ofs::ws
