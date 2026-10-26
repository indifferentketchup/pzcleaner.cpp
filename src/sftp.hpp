#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>

struct _LIBSSH2_SESSION;
struct _LIBSSH2_CHANNEL;

namespace sftp {

enum : std::uint8_t {
    kOpen = 3,
    kClose = 4,
    kRead = 5,
    kWrite = 6,
    kOpenDir = 11,
    kReadDir = 12,
    kRemove = 13,
    kMkdir = 14,
    kRmdir = 15,
    kStat = 17,
    kRename = 18,
    kExtended = 200,
    kStatus = 101,
    kHandle = 102,
    kData = 103,
    kName = 104,
    kAttrs = 105,
};

enum : std::uint32_t {
    kOk = 0,
    kEof = 1,
    kNoSuchFile = 2,
};

struct Reply {
    std::uint8_t type = 0;
    std::string body;
};

class Writer {
public:
    Writer& u32(std::uint32_t value);
    Writer& u64(std::uint64_t value);
    Writer& str(std::string_view value);
    const std::string& bytes() const { return data_; }

private:
    std::string data_;
};

class Reader {
public:
    explicit Reader(const std::string& data) : data_(data) {}
    std::uint32_t u32();
    std::uint64_t u64();
    std::string str();

private:
    const std::string& data_;
    std::size_t pos_ = 0;
};

struct UnknownHost {
    std::string fingerprint;
    std::string keyType;
};

class Link {
public:
    static std::unique_ptr<Link> open(const std::string& host, int port, const std::string& user,
                                      const std::string& password, const std::string& trust,
                                      const std::string& knownHosts, UnknownHost& unknown);
    ~Link();
    Link(const Link&) = delete;
    Link& operator=(const Link&) = delete;

    std::future<Reply> send(std::uint8_t type, const std::string& fields);
    Reply call(std::uint8_t type, const std::string& fields) { return send(type, fields).get(); }
    bool posixRename() const { return posixRename_; }

private:
    Link() = default;
    void loop();
    void fail(const std::string& message);
    void dispatchReplies();
    void waitForSocket();

    std::uintptr_t socket_ = 0;
    _LIBSSH2_SESSION* session_ = nullptr;
    _LIBSSH2_CHANNEL* channel_ = nullptr;
    bool posixRename_ = false;

    std::mutex mutex_;
    std::condition_variable wake_;
    std::string queued_;
    std::unordered_map<std::uint32_t, std::promise<Reply>> pending_;
    std::string broken_;
    bool stop_ = false;
    std::uint32_t nextId_ = 1;
    std::chrono::steady_clock::time_point lastReceived_;

    std::string out_;
    std::size_t outPos_ = 0;
    std::string in_;
    std::thread thread_;
};

}
