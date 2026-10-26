#include "sftp.hpp"

#include "base64.hpp"

#include <libssh2.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <fcntl.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace sftp {

namespace {

constexpr auto kConnectTimeout = std::chrono::seconds(20);
constexpr auto kStallTimeout = std::chrono::seconds(60);
constexpr std::size_t kWriteChunk = 32768;

#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket kNoSocket = INVALID_SOCKET;
void closeSocket(Socket s) { closesocket(s); }
void setNonBlocking(Socket s, bool on) {
    u_long mode = on ? 1 : 0;
    ioctlsocket(s, FIONBIO, &mode);
}
bool connectPending() { return WSAGetLastError() == WSAEWOULDBLOCK; }
#else
using Socket = int;
constexpr Socket kNoSocket = -1;
void closeSocket(Socket s) { close(s); }
void setNonBlocking(Socket s, bool on) {
    int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
}
bool connectPending() { return errno == EINPROGRESS; }
#endif

Socket connectSocket(const std::string& host, int port) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* found = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &found) != 0 || found == nullptr) {
        throw std::runtime_error("could not find " + host + ". Check the host in your host's panel");
    }
    Socket result = kNoSocket;
    for (addrinfo* a = found; a != nullptr && result == kNoSocket; a = a->ai_next) {
        Socket s = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == kNoSocket) {
            continue;
        }
        setNonBlocking(s, true);
        bool ok = ::connect(s, a->ai_addr, static_cast<int>(a->ai_addrlen)) == 0;
        if (!ok && connectPending()) {
            fd_set writable;
            FD_ZERO(&writable);
            FD_SET(s, &writable);
            timeval tv{static_cast<long>(kConnectTimeout.count()), 0};
            if (select(static_cast<int>(s) + 1, nullptr, &writable, nullptr, &tv) == 1) {
                int error = 0;
                socklen_t length = sizeof(error);
                getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &length);
                ok = error == 0;
            }
        }
        if (ok) {
            setNonBlocking(s, false);
            result = s;
        } else {
            closeSocket(s);
        }
    }
    freeaddrinfo(found);
    if (result == kNoSocket) {
        throw std::runtime_error("could not reach " + host + " on port " + std::to_string(port) +
                                 ". Check the host and port in your host's panel");
    }
    return result;
}

std::string sessionError(LIBSSH2_SESSION* session) {
    char* message = nullptr;
    libssh2_session_last_error(session, &message, nullptr, 0);
    return message ? message : "unknown error";
}

struct KeyInfo {
    int knownHostBit;
    const char* name;
};

KeyInfo keyInfo(int type) {
    switch (type) {
    case LIBSSH2_HOSTKEY_TYPE_RSA:
        return {LIBSSH2_KNOWNHOST_KEY_SSHRSA, "ssh-rsa"};
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_256:
        return {LIBSSH2_KNOWNHOST_KEY_ECDSA_256, "ecdsa-sha2-nistp256"};
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_384:
        return {LIBSSH2_KNOWNHOST_KEY_ECDSA_384, "ecdsa-sha2-nistp384"};
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_521:
        return {LIBSSH2_KNOWNHOST_KEY_ECDSA_521, "ecdsa-sha2-nistp521"};
    case LIBSSH2_HOSTKEY_TYPE_ED25519:
        return {LIBSSH2_KNOWNHOST_KEY_ED25519, "ssh-ed25519"};
    default:
        return {LIBSSH2_KNOWNHOST_KEY_UNKNOWN, "unknown"};
    }
}

std::string password;

void answerPrompts(const char*, int, const char*, int, int count, const LIBSSH2_USERAUTH_KBDINT_PROMPT*,
                   LIBSSH2_USERAUTH_KBDINT_RESPONSE* responses, void**) {
    for (int i = 0; i < count; ++i) {
        responses[i].text = static_cast<char*>(std::malloc(password.size()));
        std::memcpy(responses[i].text, password.data(), password.size());
        responses[i].length = static_cast<unsigned int>(password.size());
    }
}

void checkHostKey(LIBSSH2_SESSION* session, const std::string& host, int port, const std::string& trust,
                  const std::string& knownHosts, UnknownHost& unknown) {
    std::size_t keyLength = 0;
    int keyType = 0;
    const char* key = libssh2_session_hostkey(session, &keyLength, &keyType);
    const char* hash = libssh2_hostkey_hash(session, LIBSSH2_HOSTKEY_HASH_SHA256);
    if (key == nullptr || hash == nullptr) {
        throw std::runtime_error("the server sent no host key");
    }
    KeyInfo info = keyInfo(keyType);
    std::string fingerprint = "SHA256:" + base64Encode(std::string_view(hash, 32), false);
    LIBSSH2_KNOWNHOSTS* hosts = libssh2_knownhost_init(session);
    libssh2_knownhost_readfile(hosts, knownHosts.c_str(), LIBSSH2_KNOWNHOST_FILE_OPENSSH);
    int mask = LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW | info.knownHostBit;
    libssh2_knownhost* match = nullptr;
    int check = libssh2_knownhost_checkp(hosts, host.c_str(), port, key, keyLength, mask, &match);
    if (check == LIBSSH2_KNOWNHOST_CHECK_MISMATCH) {
        libssh2_knownhost_free(hosts);
        throw std::runtime_error("the server's identity changed since the last connection. If your host reinstalled "
                                 "or moved the server, delete " + knownHosts + " and connect again");
    }
    if (check != LIBSSH2_KNOWNHOST_CHECK_MATCH) {
        if (trust != fingerprint) {
            libssh2_knownhost_free(hosts);
            unknown = {fingerprint, info.name};
            return;
        }
        std::string name = port == 22 ? host : "[" + host + "]:" + std::to_string(port);
        libssh2_knownhost_addc(hosts, name.c_str(), nullptr, key, keyLength, nullptr, 0, mask, nullptr);
        std::error_code ignored;
        std::filesystem::create_directories(std::filesystem::path(knownHosts).parent_path(), ignored);
        if (libssh2_knownhost_writefile(hosts, knownHosts.c_str(), LIBSSH2_KNOWNHOST_FILE_OPENSSH) != 0) {
            libssh2_knownhost_free(hosts);
            throw std::runtime_error("could not save the server's key to " + knownHosts);
        }
    }
    libssh2_knownhost_free(hosts);
}

void authenticate(LIBSSH2_SESSION* session, const std::string& user, const std::string& secret) {
    char* methods = libssh2_userauth_list(session, user.c_str(), static_cast<unsigned int>(user.size()));
    if (methods == nullptr) {
        if (libssh2_userauth_authenticated(session)) {
            return;
        }
        throw std::runtime_error("the server did not accept that user and password");
    }
    bool ok = false;
    if (std::strstr(methods, "password") != nullptr) {
        ok = libssh2_userauth_password(session, user.c_str(), secret.c_str()) == 0;
    }
    if (!ok && std::strstr(methods, "keyboard-interactive") != nullptr) {
        password = secret;
        ok = libssh2_userauth_keyboard_interactive(session, user.c_str(), answerPrompts) == 0;
        password.clear();
    }
    if (!ok) {
        throw std::runtime_error("the server did not accept that user and password");
    }
}

void writeAll(LIBSSH2_CHANNEL* channel, const std::string& data) {
    std::size_t pos = 0;
    while (pos < data.size()) {
        auto n = libssh2_channel_write(channel, data.data() + pos, data.size() - pos);
        if (n < 0) {
            throw std::runtime_error("the server closed the sftp session");
        }
        pos += static_cast<std::size_t>(n);
    }
}

std::string readExactly(LIBSSH2_CHANNEL* channel, std::size_t size) {
    std::string data(size, '\0');
    std::size_t pos = 0;
    while (pos < size) {
        auto n = libssh2_channel_read(channel, data.data() + pos, size - pos);
        if (n <= 0) {
            throw std::runtime_error("the server did not start an sftp session");
        }
        pos += static_cast<std::size_t>(n);
    }
    return data;
}

std::uint32_t readU32(const std::string& data, std::size_t pos) {
    return (static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[pos])) << 24) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[pos + 1])) << 16) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[pos + 2])) << 8) |
           static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[pos + 3]));
}

bool startSftp(LIBSSH2_CHANNEL* channel) {
    Writer init;
    init.u32(5);
    std::string packet = init.bytes() + std::string(1, '\x01');
    Writer version;
    version.u32(3);
    writeAll(channel, packet + version.bytes());
    std::uint32_t length = readU32(readExactly(channel, 4), 0);
    if (length < 5 || length > (1u << 20)) {
        throw std::runtime_error("the server did not start an sftp session");
    }
    std::string body = readExactly(channel, length);
    if (static_cast<std::uint8_t>(body[0]) != 2) {
        throw std::runtime_error("the server did not start an sftp session");
    }
    std::string rest = body.substr(5);
    Reader reader(rest);
    bool posixRename = false;
    try {
        for (;;) {
            std::string name = reader.str();
            reader.str();
            if (name == "posix-rename@openssh.com") {
                posixRename = true;
            }
        }
    } catch (const std::exception&) {
    }
    return posixRename;
}

}

Writer& Writer::u32(std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        data_ += static_cast<char>((value >> shift) & 0xff);
    }
    return *this;
}

Writer& Writer::u64(std::uint64_t value) {
    u32(static_cast<std::uint32_t>(value >> 32));
    return u32(static_cast<std::uint32_t>(value));
}

Writer& Writer::str(std::string_view value) {
    u32(static_cast<std::uint32_t>(value.size()));
    data_.append(value);
    return *this;
}

std::uint32_t Reader::u32() {
    if (pos_ + 4 > data_.size()) {
        throw std::runtime_error("short sftp reply");
    }
    std::uint32_t value = readU32(data_, pos_);
    pos_ += 4;
    return value;
}

std::uint64_t Reader::u64() {
    std::uint64_t high = u32();
    return (high << 32) | u32();
}

std::string Reader::str() {
    std::uint32_t length = u32();
    if (pos_ + length > data_.size()) {
        throw std::runtime_error("short sftp reply");
    }
    std::string value = data_.substr(pos_, length);
    pos_ += length;
    return value;
}

std::unique_ptr<Link> Link::open(const std::string& host, int port, const std::string& user,
                                 const std::string& secret, const std::string& trust, const std::string& knownHosts,
                                 UnknownHost& unknown) {
    std::unique_ptr<Link> link(new Link());
    Socket s = connectSocket(host, port);
    link->socket_ = static_cast<std::uintptr_t>(s);
    link->session_ = libssh2_session_init();
    if (link->session_ == nullptr) {
        throw std::runtime_error("could not start an ssh session");
    }
    libssh2_session_set_blocking(link->session_, 1);
    libssh2_session_set_timeout(link->session_, static_cast<long>(kConnectTimeout.count() * 1000));
    if (libssh2_session_handshake(link->session_, static_cast<libssh2_socket_t>(s)) != 0) {
        throw std::runtime_error("could not start an ssh session with " + host + ": " + sessionError(link->session_));
    }
    checkHostKey(link->session_, host, port, trust, knownHosts, unknown);
    if (!unknown.fingerprint.empty()) {
        return nullptr;
    }
    authenticate(link->session_, user, secret);
    link->channel_ = libssh2_channel_open_session(link->session_);
    if (link->channel_ == nullptr ||
        libssh2_channel_subsystem(link->channel_, "sftp") != 0) {
        throw std::runtime_error("the server did not start an sftp session");
    }
    link->posixRename_ = startSftp(link->channel_);
    libssh2_keepalive_config(link->session_, 1, 15);
    libssh2_session_set_blocking(link->session_, 0);
    link->lastReceived_ = std::chrono::steady_clock::now();
    link->thread_ = std::thread([raw = link.get()] { raw->loop(); });
    return link;
}

Link::~Link() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
    fail("the connection was closed");
    if (session_ != nullptr) {
        libssh2_session_set_blocking(session_, 1);
        libssh2_session_set_timeout(session_, 2000);
        if (channel_ != nullptr) {
            libssh2_channel_close(channel_);
            libssh2_channel_free(channel_);
        }
        libssh2_session_disconnect(session_, "bye");
        libssh2_session_free(session_);
    }
    if (socket_ != 0) {
        closeSocket(static_cast<Socket>(socket_));
    }
}

std::future<Reply> Link::send(std::uint8_t type, const std::string& fields) {
    std::promise<Reply> promise;
    std::future<Reply> future = promise.get_future();
    {
        std::lock_guard lock(mutex_);
        if (!broken_.empty()) {
            promise.set_exception(std::make_exception_ptr(std::runtime_error(broken_)));
            return future;
        }
        std::uint32_t id = nextId_++;
        Writer header;
        header.u32(static_cast<std::uint32_t>(fields.size() + 5));
        queued_ += header.bytes();
        queued_ += static_cast<char>(type);
        Writer idField;
        idField.u32(id);
        queued_ += idField.bytes();
        queued_ += fields;
        if (pending_.empty()) {
            lastReceived_ = std::chrono::steady_clock::now();
        }
        pending_.emplace(id, std::move(promise));
    }
    wake_.notify_one();
    return future;
}

void Link::fail(const std::string& message) {
    std::unordered_map<std::uint32_t, std::promise<Reply>> failed;
    {
        std::lock_guard lock(mutex_);
        if (broken_.empty()) {
            broken_ = message;
        }
        failed.swap(pending_);
    }
    for (auto& [id, promise] : failed) {
        promise.set_exception(std::make_exception_ptr(std::runtime_error(broken_)));
    }
}

void Link::dispatchReplies() {
    std::size_t pos = 0;
    while (in_.size() - pos >= 4) {
        std::uint32_t length = readU32(in_, pos);
        if (in_.size() - pos - 4 < length) {
            break;
        }
        if (length >= 5) {
            Reply reply;
            reply.type = static_cast<std::uint8_t>(in_[pos + 4]);
            std::uint32_t id = readU32(in_, pos + 5);
            reply.body = in_.substr(pos + 9, length - 5);
            std::promise<Reply> promise;
            bool found = false;
            {
                std::lock_guard lock(mutex_);
                auto it = pending_.find(id);
                if (it != pending_.end()) {
                    promise = std::move(it->second);
                    pending_.erase(it);
                    found = true;
                }
            }
            if (found) {
                promise.set_value(std::move(reply));
            }
        }
        pos += 4 + length;
    }
    in_.erase(0, pos);
}

void Link::waitForSocket() {
    Socket s = static_cast<Socket>(socket_);
    fd_set readable;
    fd_set writable;
    FD_ZERO(&readable);
    FD_ZERO(&writable);
    FD_SET(s, &readable);
    bool outbound = (libssh2_session_block_directions(session_) & LIBSSH2_SESSION_BLOCK_OUTBOUND) != 0;
    if (outbound) {
        FD_SET(s, &writable);
    }
    timeval tv{0, 2000};
    select(static_cast<int>(s) + 1, &readable, outbound ? &writable : nullptr, nullptr, &tv);
}

void Link::loop() {
    std::vector<char> buffer(65536);
    for (;;) {
        {
            std::unique_lock lock(mutex_);
            if (stop_) {
                return;
            }
            if (outPos_ == out_.size() && queued_.empty() && pending_.empty()) {
                wake_.wait_for(lock, std::chrono::seconds(1), [this] { return stop_ || !queued_.empty(); });
                if (stop_) {
                    return;
                }
            }
            if (outPos_ == out_.size() && !queued_.empty()) {
                out_.swap(queued_);
                queued_.clear();
                outPos_ = 0;
            }
        }
        bool progressed = false;
        while (outPos_ < out_.size()) {
            auto n = libssh2_channel_write(channel_, out_.data() + outPos_, std::min(out_.size() - outPos_, kWriteChunk));
            if (n == LIBSSH2_ERROR_EAGAIN) {
                break;
            }
            if (n < 0) {
                fail("the connection to the server was lost: " + sessionError(session_));
                return;
            }
            outPos_ += static_cast<std::size_t>(n);
            progressed = true;
        }
        if (outPos_ == out_.size()) {
            out_.clear();
            outPos_ = 0;
        }
        for (;;) {
            auto n = libssh2_channel_read(channel_, buffer.data(), buffer.size());
            if (n == LIBSSH2_ERROR_EAGAIN) {
                break;
            }
            if (n < 0) {
                fail("the connection to the server was lost: " + sessionError(session_));
                return;
            }
            if (n == 0) {
                if (libssh2_channel_eof(channel_)) {
                    fail("the server closed the connection");
                    return;
                }
                break;
            }
            in_.append(buffer.data(), static_cast<std::size_t>(n));
            progressed = true;
            std::lock_guard lock(mutex_);
            lastReceived_ = std::chrono::steady_clock::now();
        }
        dispatchReplies();
        int next = 0;
        libssh2_keepalive_send(session_, &next);
        bool stalled = false;
        {
            std::lock_guard lock(mutex_);
            stalled = !pending_.empty() && std::chrono::steady_clock::now() - lastReceived_ > kStallTimeout;
        }
        if (stalled) {
            fail("the server stopped responding");
            return;
        }
        if (!progressed) {
            waitForSocket();
        }
    }
}

}
