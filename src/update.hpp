#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace update {

using Progress = std::function<void(std::uint64_t done, std::uint64_t total)>;

const char* currentVersion();
bool isRelease();
std::string latestTag();
bool isNewer(const std::string& tag);
std::string releasePage(const std::string& tag);
void install(const std::string& tag, const Progress& progress);
bool relaunch();
void cleanUp();

}
