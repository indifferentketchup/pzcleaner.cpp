#pragma once

#include <cstdint>
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
