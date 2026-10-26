#pragma once

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

inline std::string base64Encode(std::string_view data, bool pad = true) {
    static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 2 < data.size(); i += 3) {
        std::uint32_t n = (static_cast<std::uint8_t>(data[i]) << 16) | (static_cast<std::uint8_t>(data[i + 1]) << 8) |
                          static_cast<std::uint8_t>(data[i + 2]);
        out += kAlphabet[n >> 18];
        out += kAlphabet[(n >> 12) & 63];
        out += kAlphabet[(n >> 6) & 63];
        out += kAlphabet[n & 63];
    }
    std::size_t rest = data.size() - i;
    if (rest > 0) {
        std::uint32_t n = static_cast<std::uint8_t>(data[i]) << 16;
        if (rest == 2) {
            n |= static_cast<std::uint8_t>(data[i + 1]) << 8;
        }
        out += kAlphabet[n >> 18];
        out += kAlphabet[(n >> 12) & 63];
        if (rest == 2) {
            out += kAlphabet[(n >> 6) & 63];
        }
        if (pad) {
            out.append(3 - rest, '=');
        }
    }
    return out;
}

inline std::string base64Decode(std::string_view text) {
    static const std::array<int, 256> kValues = [] {
        std::array<int, 256> values{};
        values.fill(-1);
        const std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (std::size_t i = 0; i < alphabet.size(); ++i) {
            values[static_cast<std::uint8_t>(alphabet[i])] = static_cast<int>(i);
        }
        return values;
    }();
    std::string out;
    out.reserve(text.size() / 4 * 3);
    std::uint32_t buffer = 0;
    int bits = 0;
    for (char c : text) {
        if (c == '=') {
            break;
        }
        int value = kValues[static_cast<std::uint8_t>(c)];
        if (value < 0) {
            throw std::runtime_error("bad base64 data");
        }
        buffer = (buffer << 6) | static_cast<std::uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((buffer >> bits) & 0xff);
        }
    }
    return out;
}
