#include "update.hpp"

#include <httplib.h>
#include <mbedtls/sha256.h>

#include <array>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
extern char** environ;
#endif

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace fs = std::filesystem;

namespace update {

namespace {

const char* const kHost = "https://github.com";
const std::string kRepo = "/indifferentketchup/pzcleaner.cpp";

#if defined(_WIN32)
const char* const kAsset = "pzcleaner-windows-amd64.exe";
#elif defined(__APPLE__)
const char* const kAsset = "pzcleaner-macos.zip";
#else
const char* const kAsset = "pzcleaner-x86_64.AppImage";
#endif

std::optional<std::array<int, 3>> parseVersion(std::string_view text) {
    if (!text.empty() && text[0] == 'v') {
        text.remove_prefix(1);
    }
    std::array<int, 3> parts{};
    for (std::size_t i = 0; i < parts.size(); ++i) {
        std::size_t digits = 0;
        int value = 0;
        while (digits < text.size() && digits < 6 && std::isdigit(static_cast<unsigned char>(text[digits]))) {
            value = value * 10 + (text[digits] - '0');
            ++digits;
        }
        if (digits == 0) {
            return std::nullopt;
        }
        parts[i] = value;
        text.remove_prefix(digits);
        if (i + 1 < parts.size()) {
            if (text.empty() || text[0] != '.') {
                return std::nullopt;
            }
            text.remove_prefix(1);
        }
    }
    if (!text.empty()) {
        return std::nullopt;
    }
    return parts;
}

httplib::Client client(bool follow) {
    httplib::Client client(kHost);
    client.set_connection_timeout(15);
    client.set_read_timeout(60);
    client.set_follow_location(follow);
    return client;
}

httplib::Headers headers() { return {{"User-Agent", std::string("pzcleaner/") + currentVersion()}}; }

std::string download(const std::string& path, const Progress& progress) {
    httplib::Client github = client(true);
    httplib::Result result = github.Get(path, headers(), [&progress](std::size_t done, std::size_t total) {
        if (progress) {
            progress(done, total);
        }
        return true;
    });
    if (!result) {
        throw std::runtime_error("the download stopped (" + httplib::to_string(result.error()) + ")");
    }
    if (result->status != 200) {
        throw std::runtime_error("GitHub answered " + std::to_string(result->status) + " for " + path);
    }
    return std::move(result->body);
}

std::string sha256Hex(const std::string& data) {
    std::array<unsigned char, 32> digest{};
    mbedtls_sha256(reinterpret_cast<const unsigned char*>(data.data()), data.size(), digest.data(), 0);
    std::string hex;
    for (unsigned char byte : digest) {
        const char* digits = "0123456789abcdef";
        hex += digits[byte >> 4];
        hex += digits[byte & 15];
    }
    return hex;
}

std::string expectedHash(const std::string& sums) {
    std::istringstream lines(sums);
    std::string hash;
    std::string name;
    while (lines >> hash >> name) {
        if (!name.empty() && name[0] == '*') {
            name.erase(0, 1);
        }
        if (name == kAsset) {
            for (char& c : hash) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            return hash;
        }
    }
    throw std::runtime_error(std::string("the release has no checksum for ") + kAsset);
}

void writeFile(const fs::path& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.close();
    if (!out) {
        throw std::runtime_error("could not write " + path.string());
    }
}

#ifndef _WIN32
bool spawn(const std::vector<std::string>& args, bool wait) {
    std::vector<char*> argv;
    for (const std::string& arg : args) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    if (!wait) {
        posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID);
    }
    pid_t pid = 0;
    int rc = posix_spawn(&pid, argv[0], nullptr, &attr, argv.data(), environ);
    posix_spawnattr_destroy(&attr);
    if (rc != 0) {
        return false;
    }
    if (!wait) {
        return true;
    }
    int status = 0;
    return waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
#endif

#if defined(_WIN32)

fs::path exePath() {
    std::wstring buffer(32768, L'\0');
    DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    buffer.resize(length);
    return fs::path(buffer);
}

fs::path withSuffix(const fs::path& path, const wchar_t* suffix) {
    fs::path out = path;
    out += suffix;
    return out;
}

void replace(const std::string& bytes) {
    fs::path exe = exePath();
    fs::path fresh = withSuffix(exe, L".new");
    fs::path old = withSuffix(exe, L".old");
    writeFile(fresh, bytes);
    DeleteFileW(old.c_str());
    if (!MoveFileExW(exe.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(fresh.c_str());
        throw std::runtime_error("could not move the running copy aside (Windows error " +
                                 std::to_string(GetLastError()) + ")");
    }
    if (!MoveFileExW(fresh.c_str(), exe.c_str(), 0)) {
        DWORD error = GetLastError();
        MoveFileExW(old.c_str(), exe.c_str(), 0);
        throw std::runtime_error("could not put the new copy in place (Windows error " + std::to_string(error) + ")");
    }
}

bool launch() {
    fs::path exe = exePath();
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(exe.c_str(), nullptr, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) {
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

void removeLeftovers() {
    DeleteFileW(withSuffix(exePath(), L".old").c_str());
    DeleteFileW(withSuffix(exePath(), L".new").c_str());
}

#elif defined(__APPLE__)

fs::path bundlePath() {
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buffer(size, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        throw std::runtime_error("could not find where pzcleaner is");
    }
    fs::path bundle = fs::canonical(buffer.c_str()).parent_path().parent_path().parent_path();
    if (bundle.extension() != ".app") {
        throw std::runtime_error("this copy is not inside pzcleaner.app, so it can't update itself");
    }
    if (bundle.string().find("/AppTranslocation/") != std::string::npos) {
        throw std::runtime_error("macOS runs this copy from a temporary place. Move pzcleaner to the Applications "
                                 "folder and open it from there, then it can update itself");
    }
    return bundle;
}

void replace(const std::string& bytes) {
    fs::path bundle = bundlePath();
    fs::path work = bundle.parent_path() / ".pzcleaner-update";
    fs::remove_all(work);
    fs::create_directories(work);
    fs::path zip = work / "update.zip";
    writeFile(zip, bytes);
    fs::path unpacked = work / "unpacked";
    if (!spawn({"/usr/bin/ditto", "-x", "-k", zip.string(), unpacked.string()}, true)) {
        fs::remove_all(work);
        throw std::runtime_error("the update could not be unpacked");
    }
    fs::path fresh = unpacked / "pzcleaner.app";
    if (!fs::is_directory(fresh)) {
        fs::remove_all(work);
        throw std::runtime_error("the update has no pzcleaner.app in it");
    }
    fs::path old = bundle;
    old += ".old";
    fs::remove_all(old);
    fs::rename(bundle, old);
    std::error_code failed;
    fs::rename(fresh, bundle, failed);
    if (failed) {
        fs::rename(old, bundle);
        fs::remove_all(work);
        throw std::runtime_error("could not put the new copy in place: " + failed.message());
    }
    fs::remove_all(work);
}

bool launch() { return spawn({"/usr/bin/open", "-n", bundlePath().string()}, false); }

void removeLeftovers() {
    fs::path bundle = bundlePath();
    fs::path old = bundle;
    old += ".old";
    std::error_code ignored;
    fs::remove_all(old, ignored);
    fs::remove_all(bundle.parent_path() / ".pzcleaner-update", ignored);
}

#else

std::string appImage() {
    const char* image = std::getenv("APPIMAGE");
    if (image == nullptr || *image == '\0') {
        throw std::runtime_error("only the AppImage can update itself");
    }
    return image;
}

void replace(const std::string& bytes) {
    std::string target = appImage();
    std::string fresh = target + ".new";
    writeFile(fresh, bytes);
    chmod(fresh.c_str(), 0755);
    if (std::rename(fresh.c_str(), target.c_str()) != 0) {
        std::string reason = std::strerror(errno);
        std::remove(fresh.c_str());
        throw std::runtime_error("could not replace " + target + ": " + reason);
    }
}

bool launch() { return spawn({appImage()}, false); }

void removeLeftovers() {
    const char* image = std::getenv("APPIMAGE");
    if (image != nullptr && *image != '\0') {
        std::remove((std::string(image) + ".new").c_str());
    }
}

#endif

}

const char* currentVersion() { return PZCLEANER_VERSION; }

bool isRelease() { return parseVersion(currentVersion()).has_value(); }

std::string latestTag() {
    httplib::Client github = client(false);
    httplib::Result result = github.Get(kRepo + "/releases/latest", headers());
    if (!result) {
        throw std::runtime_error("could not reach GitHub (" + httplib::to_string(result.error()) + ")");
    }
    std::string location = result->get_header_value("Location");
    std::size_t at = location.find("/releases/tag/");
    if (result->status / 100 != 3 || at == std::string::npos) {
        throw std::runtime_error("GitHub did not name a latest release");
    }
    return location.substr(at + std::strlen("/releases/tag/"));
}

bool isNewer(const std::string& tag) {
    std::optional<std::array<int, 3>> latest = parseVersion(tag);
    std::optional<std::array<int, 3>> current = parseVersion(currentVersion());
    return latest && current && *latest > *current;
}

std::string releasePage(const std::string& tag) { return std::string(kHost) + kRepo + "/releases/tag/" + tag; }

void install(const std::string& tag, const Progress& progress) {
    std::string base = kRepo + "/releases/download/" + tag + "/";
    std::string hash = expectedHash(download(base + "SHA256SUMS.txt", nullptr));
    std::string bytes = download(base + kAsset, progress);
    if (sha256Hex(bytes) != hash) {
        throw std::runtime_error("the downloaded file does not match the release checksum");
    }
    replace(bytes);
}

bool relaunch() {
    try {
        return launch();
    } catch (const std::exception&) {
        return false;
    }
}

void cleanUp() {
    try {
        removeLeftovers();
    } catch (const std::exception&) {
    }
}

}
