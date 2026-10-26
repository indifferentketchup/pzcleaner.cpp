#pragma once

#include "store.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using Pairs = std::vector<std::pair<int, int>>;

inline std::int64_t chunkKey(int x, int y) {
    return (static_cast<std::int64_t>(x) << 32) ^ static_cast<std::uint32_t>(y);
}

struct ConnectRequest {
    std::string host;
    int port = 22;
    std::string user;
    std::string password;
    std::string path;
    std::string trust;
};

struct ConnectResult {
    std::string name;
    std::string fingerprint;
    std::string keyType;
};

struct ExecuteRequest {
    Pairs chunks;
    Pairs cells;
    std::vector<std::string> cellKinds;
    bool vehicles = false;
    bool trash = true;
};

struct ExecuteResult {
    std::int64_t cellFiles = 0;
    std::vector<std::int64_t> vehicleIds;
    std::string stamp;
};

class App {
public:
    using Progress = std::function<void(std::int64_t done, std::int64_t total)>;

    App(std::string knownHosts, Progress progress)
        : knownHosts_(std::move(knownHosts)), progress_(std::move(progress)) {}

    ConnectResult connect(const ConnectRequest& request);
    std::string openLocal(const std::string& dir);
    void disconnect();
    Pairs index();
    std::optional<std::string> readFile(const std::string& name);
    std::vector<std::string> listTrash();
    ExecuteResult execute(const ExecuteRequest& request);
    std::int64_t restore(const std::string& stamp);
    void emptyTrash();

private:
    struct Session {
        std::shared_ptr<Store> store;
        std::string root;
        std::string backup;
    };

    std::string open(std::shared_ptr<Store> store, const std::string& root);
    Session current();

    std::string knownHosts_;
    Progress progress_;
    std::mutex mutex_;
    Session session_;
};
