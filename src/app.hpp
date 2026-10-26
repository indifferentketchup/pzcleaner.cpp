#pragma once

#include "store.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

class NamedError : public std::runtime_error {
public:
    NamedError(std::string name, const std::string& message) : std::runtime_error(message), name(std::move(name)) {}
    std::string name;
};

class App {
public:
    using Progress = std::function<void(std::int64_t done, std::int64_t total)>;

    App(std::string knownHosts, Progress progress)
        : knownHosts_(std::move(knownHosts)), progress_(std::move(progress)) {}

    nlohmann::json connect(const nlohmann::json& request);
    nlohmann::json openLocal(const std::string& dir);
    void disconnect();
    nlohmann::json list(const std::string& rel);
    nlohmann::json stat(const std::string& rel);
    std::string read(const std::string& rel);
    void write(const std::string& rel, const std::string& base64);
    void mkdir(const std::string& rel);
    void rename(const std::string& from, const std::string& to);
    void remove(const std::string& rel, bool recursive);
    nlohmann::json index(const std::string& rel);
    nlohmann::json execute(const nlohmann::json& request);
    std::int64_t restore(const std::string& stamp);

private:
    struct Session {
        std::shared_ptr<Store> store;
        std::string root;
        std::string backup;
    };

    nlohmann::json open(std::shared_ptr<Store> store, const std::string& root);
    Session current();
    std::pair<std::shared_ptr<Store>, std::string> resolve(const std::string& rel);

    std::string knownHosts_;
    Progress progress_;
    std::mutex mutex_;
    Session session_;
};
