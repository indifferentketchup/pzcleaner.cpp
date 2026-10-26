#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

enum class Noun { Chunk, CellFile, Cell, Safehouse, Player, Member, Vehicle, Batch, File };

inline std::string formatNumber(std::int64_t value) {
    std::string digits = std::to_string(value < 0 ? -value : value);
    std::string out;
    for (std::size_t i = 0; i < digits.size(); ++i) {
        if (i > 0 && (digits.size() - i) % 3 == 0) {
            out += ',';
        }
        out += digits[i];
    }
    return value < 0 ? "-" + out : out;
}

inline std::string count(std::int64_t value, Noun noun) {
    static const char* const kNouns[][2] = {
        {"chunk file", "chunk files"},
        {"per-cell file", "per-cell files"},
        {"cell", "cells"},
        {"claimed safehouse", "claimed safehouses"},
        {"living player", "living players"},
        {"member", "members"},
        {"vehicle", "vehicles"},
        {"batch", "batches"},
        {"file", "files"},
    };
    return formatNumber(value) + " " + kNouns[static_cast<int>(noun)][value == 1 ? 0 : 1];
}

inline std::string joinList(const std::vector<std::string>& parts) {
    if (parts.size() == 2) {
        return parts[0] + " and " + parts[1];
    }
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) {
            out += i + 1 == parts.size() ? ", and " : ", ";
        }
        out += parts[i];
    }
    return out;
}

inline std::string formatStamp(const std::string& stamp) {
    constexpr std::string_view kShape = "dddd-dd-dd_dd-dd-dd";
    if (stamp.size() != kShape.size()) {
        return stamp;
    }
    for (std::size_t i = 0; i < kShape.size(); ++i) {
        bool fits = kShape[i] == 'd' ? stamp[i] >= '0' && stamp[i] <= '9' : stamp[i] == kShape[i];
        if (!fits) {
            return stamp;
        }
    }
    return stamp.substr(0, 10) + " at " + stamp.substr(11, 2) + ":" + stamp.substr(14, 2) + ":" + stamp.substr(17, 2);
}
