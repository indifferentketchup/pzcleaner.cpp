#pragma once

#include "sftp.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

constexpr int kListWorkers = 256;
constexpr int kFileWorkers = 512;

struct Entry {
    std::string name;
    bool dir = false;
    std::int64_t size = 0;
};

class FsError : public std::runtime_error {
public:
    FsError(const std::string& message, bool notFound) : std::runtime_error(message), notFound(notFound) {}
    bool notFound;
};

class Store {
public:
    virtual ~Store() = default;
    virtual std::vector<Entry> readDir(const std::string& dir) = 0;
    virtual Entry stat(const std::string& path) = 0;
    virtual std::string readFile(const std::string& path) = 0;
    virtual void writeFile(const std::string& path, const std::string& data) = 0;
    virtual void remove(const std::string& path) = 0;
    virtual void rename(const std::string& from, const std::string& to) = 0;
    virtual void replace(const std::string& from, const std::string& to) = 0;
    virtual void mkdirAll(const std::string& dir) = 0;
};

class RemoteStore : public Store {
public:
    explicit RemoteStore(std::unique_ptr<sftp::Link> link) : link_(std::move(link)) {}
    std::vector<Entry> readDir(const std::string& dir) override;
    Entry stat(const std::string& path) override;
    std::string readFile(const std::string& path) override;
    void writeFile(const std::string& path, const std::string& data) override;
    void remove(const std::string& path) override;
    void rename(const std::string& from, const std::string& to) override;
    void replace(const std::string& from, const std::string& to) override;
    void mkdirAll(const std::string& dir) override;

private:
    std::string open(const std::string& path, std::uint32_t flags);
    void close(const std::string& handle);
    std::unique_ptr<sftp::Link> link_;
};

class LocalStore : public Store {
public:
    std::vector<Entry> readDir(const std::string& dir) override;
    Entry stat(const std::string& path) override;
    std::string readFile(const std::string& path) override;
    void writeFile(const std::string& path, const std::string& data) override;
    void remove(const std::string& path) override;
    void rename(const std::string& from, const std::string& to) override;
    void replace(const std::string& from, const std::string& to) override;
    void mkdirAll(const std::string& dir) override;
};

std::string joinPath(const std::string& a, const std::string& b);
std::string parentPath(const std::string& path);
std::string baseName(const std::string& path);

template <typename T>
void runPool(const std::vector<T>& items, int workers, const std::function<void(const T&)>& task) {
    std::atomic<std::size_t> next{0};
    std::atomic<bool> failed{false};
    std::exception_ptr first;
    std::mutex mutex;
    std::vector<std::thread> threads;
    int count = static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(workers), items.size()));
    threads.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        threads.emplace_back([&] {
            for (;;) {
                std::size_t index = next++;
                if (index >= items.size() || failed) {
                    return;
                }
                try {
                    task(items[index]);
                } catch (...) {
                    std::lock_guard lock(mutex);
                    if (!first) {
                        first = std::current_exception();
                    }
                    failed = true;
                    return;
                }
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    if (first) {
        std::rethrow_exception(first);
    }
}

struct Tree {
    std::vector<std::string> files;
    std::vector<std::vector<std::string>> levels;
};

Tree walk(Store& store, const std::string& root);
void removeTree(Store& store, const std::string& dir);
