#include "store.hpp"

#include <deque>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>

namespace fs = std::filesystem;

namespace {

constexpr std::uint32_t kChunk = 32768;
constexpr std::size_t kInFlight = 64;
constexpr std::uint32_t kReadFlag = 0x01;
constexpr std::uint32_t kWriteFlags = 0x02 | 0x08 | 0x10;

[[noreturn]] void throwStatus(const sftp::Reply& reply, const std::string& path) {
    if (reply.type != sftp::kStatus) {
        throw FsError("unexpected sftp reply for " + path, false);
    }
    sftp::Reader reader(reply.body);
    std::uint32_t code = reader.u32();
    std::string message;
    try {
        message = reader.str();
    } catch (const std::exception&) {
    }
    if (message.empty()) {
        message = code == sftp::kNoSuchFile ? "no such file" : "failed";
    }
    throw FsError(path + ": " + message, code == sftp::kNoSuchFile);
}

void expectOk(const sftp::Reply& reply, const std::string& path) {
    if (reply.type == sftp::kStatus) {
        sftp::Reader reader(reply.body);
        if (reader.u32() == sftp::kOk) {
            return;
        }
    }
    throwStatus(reply, path);
}

std::uint32_t statusCode(const sftp::Reply& reply) {
    if (reply.type != sftp::kStatus) {
        return 0xffffffff;
    }
    sftp::Reader reader(reply.body);
    return reader.u32();
}

Entry readAttrs(sftp::Reader& reader, const std::string& name, const std::string& longName) {
    Entry entry;
    entry.name = name;
    std::uint32_t flags = reader.u32();
    bool hasPermissions = false;
    if (flags & 0x01) {
        entry.size = static_cast<std::int64_t>(reader.u64());
    }
    if (flags & 0x02) {
        reader.u32();
        reader.u32();
    }
    if (flags & 0x04) {
        entry.dir = (reader.u32() & 0170000) == 0040000;
        hasPermissions = true;
    }
    if (flags & 0x08) {
        reader.u32();
        reader.u32();
    }
    if (flags & 0x80000000u) {
        std::uint32_t count = reader.u32();
        for (std::uint32_t i = 0; i < count; ++i) {
            reader.str();
            reader.str();
        }
    }
    if (!hasPermissions && !longName.empty()) {
        entry.dir = longName[0] == 'd';
    }
    return entry;
}

fs::path localPath(const std::string& path) {
    return fs::path(std::u8string(path.begin(), path.end()));
}

std::string utf8(const fs::path& path) {
    std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

[[noreturn]] void throwLocal(const std::error_code& error, const std::string& path) {
    throw FsError(path + ": " + error.message(), error == std::errc::no_such_file_or_directory);
}

}

std::string joinPath(const std::string& a, const std::string& b) {
    if (a.empty()) {
        return b;
    }
    if (a.back() == '/') {
        return a + b;
    }
    return a + "/" + b;
}

std::string parentPath(const std::string& path) {
    std::size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) {
        return ".";
    }
    if (slash == 0) {
        return "/";
    }
    return path.substr(0, slash);
}

std::string baseName(const std::string& path) {
    std::size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string RemoteStore::open(const std::string& path, std::uint32_t flags) {
    sftp::Writer fields;
    fields.str(path).u32(flags).u32(0);
    sftp::Reply reply = link_->call(sftp::kOpen, fields.bytes());
    if (reply.type != sftp::kHandle) {
        throwStatus(reply, path);
    }
    sftp::Reader reader(reply.body);
    return reader.str();
}

void RemoteStore::close(const std::string& handle) {
    sftp::Writer fields;
    fields.str(handle);
    link_->call(sftp::kClose, fields.bytes());
}

std::vector<Entry> RemoteStore::readDir(const std::string& dir) {
    sftp::Writer openFields;
    openFields.str(dir);
    sftp::Reply opened = link_->call(sftp::kOpenDir, openFields.bytes());
    if (opened.type != sftp::kHandle) {
        throwStatus(opened, dir);
    }
    sftp::Reader handleReader(opened.body);
    std::string handle = handleReader.str();
    std::vector<Entry> entries;
    sftp::Writer readFields;
    readFields.str(handle);
    for (;;) {
        sftp::Reply reply = link_->call(sftp::kReadDir, readFields.bytes());
        if (reply.type == sftp::kStatus) {
            if (statusCode(reply) == sftp::kEof) {
                break;
            }
            close(handle);
            throwStatus(reply, dir);
        }
        sftp::Reader reader(reply.body);
        std::uint32_t count = reader.u32();
        for (std::uint32_t i = 0; i < count; ++i) {
            std::string name = reader.str();
            std::string longName = reader.str();
            Entry entry = readAttrs(reader, name, longName);
            if (name != "." && name != "..") {
                entries.push_back(std::move(entry));
            }
        }
    }
    close(handle);
    return entries;
}

Entry RemoteStore::stat(const std::string& path) {
    sftp::Writer fields;
    fields.str(path);
    sftp::Reply reply = link_->call(sftp::kStat, fields.bytes());
    if (reply.type != sftp::kAttrs) {
        throwStatus(reply, path);
    }
    sftp::Reader reader(reply.body);
    return readAttrs(reader, baseName(path), "");
}

std::string RemoteStore::readFile(const std::string& path) {
    std::uint64_t size = static_cast<std::uint64_t>(stat(path).size);
    std::string handle = open(path, kReadFlag);
    std::string data(size, '\0');
    std::uint64_t got = 0;
    std::deque<std::pair<std::uint64_t, std::future<sftp::Reply>>> inFlight;
    std::uint64_t nextOffset = 0;
    auto issue = [&] {
        sftp::Writer fields;
        fields.str(handle).u64(nextOffset).u32(kChunk);
        inFlight.emplace_back(nextOffset, link_->send(sftp::kRead, fields.bytes()));
        nextOffset += kChunk;
    };
    try {
        while (nextOffset < size && inFlight.size() < kInFlight) {
            issue();
        }
        while (!inFlight.empty()) {
            auto [offset, future] = std::move(inFlight.front());
            inFlight.pop_front();
            sftp::Reply reply = future.get();
            if (reply.type == sftp::kData) {
                sftp::Reader reader(reply.body);
                std::string chunk = reader.str();
                std::uint64_t want = std::min<std::uint64_t>(kChunk, size - offset);
                std::uint64_t pos = offset;
                while (true) {
                    std::uint64_t take = std::min<std::uint64_t>(chunk.size(), size - pos);
                    data.replace(pos, take, chunk, 0, take);
                    got += take;
                    pos += take;
                    if (pos >= offset + want || chunk.empty()) {
                        break;
                    }
                    sftp::Writer more;
                    more.str(handle).u64(pos).u32(static_cast<std::uint32_t>(offset + want - pos));
                    sftp::Reply extra = link_->call(sftp::kRead, more.bytes());
                    if (extra.type != sftp::kData) {
                        break;
                    }
                    sftp::Reader extraReader(extra.body);
                    chunk = extraReader.str();
                }
            } else if (statusCode(reply) != sftp::kEof) {
                throwStatus(reply, path);
            }
            if (nextOffset < size) {
                issue();
            }
        }
    } catch (...) {
        for (auto& pending : inFlight) {
            try {
                pending.second.get();
            } catch (...) {
            }
        }
        close(handle);
        throw;
    }
    close(handle);
    data.resize(static_cast<std::size_t>(std::min<std::uint64_t>(got, size)));
    return data;
}

void RemoteStore::writeFile(const std::string& path, const std::string& data) {
    std::string handle = open(path, kWriteFlags);
    std::deque<std::future<sftp::Reply>> inFlight;
    std::exception_ptr error;
    auto settle = [&] {
        try {
            expectOk(inFlight.front().get(), path);
        } catch (...) {
            if (!error) {
                error = std::current_exception();
            }
        }
        inFlight.pop_front();
    };
    for (std::size_t offset = 0; offset < data.size() && !error; offset += kChunk) {
        sftp::Writer fields;
        fields.str(handle).u64(offset).str(std::string_view(data).substr(offset, kChunk));
        inFlight.push_back(link_->send(sftp::kWrite, fields.bytes()));
        if (inFlight.size() >= kInFlight) {
            settle();
        }
    }
    while (!inFlight.empty()) {
        settle();
    }
    sftp::Writer fields;
    fields.str(handle);
    sftp::Reply closed = link_->call(sftp::kClose, fields.bytes());
    if (error) {
        std::rethrow_exception(error);
    }
    expectOk(closed, path);
}

void RemoteStore::remove(const std::string& path) {
    sftp::Writer fields;
    fields.str(path);
    sftp::Reply removed = link_->call(sftp::kRemove, fields.bytes());
    std::uint32_t code = statusCode(removed);
    if (code == sftp::kOk) {
        return;
    }
    if (code == sftp::kNoSuchFile) {
        throwStatus(removed, path);
    }
    expectOk(link_->call(sftp::kRmdir, fields.bytes()), path);
}

void RemoteStore::rename(const std::string& from, const std::string& to) {
    sftp::Writer fields;
    fields.str(from).str(to);
    expectOk(link_->call(sftp::kRename, fields.bytes()), from);
}

void RemoteStore::replace(const std::string& from, const std::string& to) {
    if (link_->posixRename()) {
        sftp::Writer fields;
        fields.str("posix-rename@openssh.com").str(from).str(to);
        expectOk(link_->call(sftp::kExtended, fields.bytes()), from);
        return;
    }
    try {
        remove(to);
    } catch (const FsError& error) {
        if (!error.notFound) {
            throw;
        }
    }
    rename(from, to);
}

void RemoteStore::mkdirAll(const std::string& dir) {
    try {
        if (stat(dir).dir) {
            return;
        }
        throw FsError(dir + ": not a folder", false);
    } catch (const FsError& error) {
        if (!error.notFound) {
            throw;
        }
    }
    std::string parent = parentPath(dir);
    if (parent != dir && parent != "/" && parent != ".") {
        mkdirAll(parent);
    }
    sftp::Writer fields;
    fields.str(dir).u32(0);
    sftp::Reply reply = link_->call(sftp::kMkdir, fields.bytes());
    if (statusCode(reply) == sftp::kOk) {
        return;
    }
    if (stat(dir).dir) {
        return;
    }
    throwStatus(reply, dir);
}

std::vector<Entry> LocalStore::readDir(const std::string& dir) {
    std::error_code error;
    fs::directory_iterator it(localPath(dir), error);
    if (error) {
        throwLocal(error, dir);
    }
    std::vector<Entry> entries;
    for (const auto& item : it) {
        std::error_code ignored;
        entries.push_back({utf8(item.path().filename()), item.is_directory(ignored), 0});
    }
    return entries;
}

Entry LocalStore::stat(const std::string& path) {
    std::error_code error;
    fs::file_status status = fs::status(localPath(path), error);
    if (error || !fs::exists(status)) {
        throwLocal(error ? error : std::make_error_code(std::errc::no_such_file_or_directory), path);
    }
    Entry entry{baseName(path), fs::is_directory(status), 0};
    if (!entry.dir) {
        entry.size = static_cast<std::int64_t>(fs::file_size(localPath(path), error));
    }
    return entry;
}

std::string LocalStore::readFile(const std::string& path) {
    std::ifstream in(localPath(path), std::ios::binary);
    if (!in) {
        stat(path);
        throw FsError(path + ": could not be read", false);
    }
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void LocalStore::writeFile(const std::string& path, const std::string& data) {
    std::ofstream out(localPath(path), std::ios::binary | std::ios::trunc);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    if (!out) {
        throw FsError(path + ": could not be written", false);
    }
}

void LocalStore::remove(const std::string& path) {
    std::error_code error;
    if (!fs::remove(localPath(path), error)) {
        throwLocal(error ? error : std::make_error_code(std::errc::no_such_file_or_directory), path);
    }
}

void LocalStore::rename(const std::string& from, const std::string& to) {
    std::error_code error;
    if (fs::exists(localPath(to), error)) {
        throw FsError(to + ": already exists", false);
    }
    fs::rename(localPath(from), localPath(to), error);
    if (error) {
        throwLocal(error, from);
    }
}

void LocalStore::replace(const std::string& from, const std::string& to) {
    std::error_code error;
    fs::rename(localPath(from), localPath(to), error);
    if (error) {
        throwLocal(error, from);
    }
}

void LocalStore::mkdirAll(const std::string& dir) {
    std::error_code error;
    fs::create_directories(localPath(dir), error);
    if (error) {
        throwLocal(error, dir);
    }
}

Tree walk(Store& store, const std::string& root) {
    Tree tree;
    tree.levels.push_back({root});
    std::mutex mutex;
    for (;;) {
        std::vector<std::string> next;
        std::function<void(const std::string&)> list = [&](const std::string& dir) {
            std::vector<Entry> entries = store.readDir(dir);
            std::lock_guard lock(mutex);
            for (const Entry& entry : entries) {
                (entry.dir ? next : tree.files).push_back(joinPath(dir, entry.name));
            }
        };
        runPool(tree.levels.back(), kListWorkers, list);
        if (next.empty()) {
            return tree;
        }
        tree.levels.push_back(std::move(next));
    }
}

void removeTree(Store& store, const std::string& dir) {
    Tree tree = walk(store, dir);
    std::function<void(const std::string&)> removeFile = [&](const std::string& file) {
        try {
            store.remove(file);
        } catch (const FsError& error) {
            if (!error.notFound) {
                throw;
            }
        }
    };
    runPool(tree.files, kFileWorkers, removeFile);
    std::function<void(const std::string&)> removeDir = [&](const std::string& path) { store.remove(path); };
    for (std::size_t i = tree.levels.size() - 1; i > 0; --i) {
        runPool(tree.levels[i], kFileWorkers, removeDir);
    }
}
