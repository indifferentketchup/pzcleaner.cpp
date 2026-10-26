#pragma once

#include <cstddef>
#include <string_view>

struct Asset {
    std::string_view path;
    const unsigned char* data;
    std::size_t size;
};

extern const Asset kAssets[];
extern const std::size_t kAssetCount;
