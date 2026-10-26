#pragma once

#include <cstddef>
#include <string_view>

struct Asset {
    std::string_view name;
    const unsigned char* data;
    std::size_t size;
};

extern const Asset kAssets[];
extern const std::size_t kAssetCount;

inline const Asset* findAsset(std::string_view name) {
    for (std::size_t i = 0; i < kAssetCount; ++i) {
        if (kAssets[i].name == name) {
            return &kAssets[i];
        }
    }
    return nullptr;
}
