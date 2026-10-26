#include "app.hpp"

#include "base64.hpp"

#include <sqlite3.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <iomanip>
#include <set>
#include <sstream>
#include <thread>
#include <unordered_set>

using nlohmann::json;

namespace {

const std::string kTrashDir = "_pzcleaner_trash";
const std::string kBackupFolder = "pzcleaner-backup";
const std::string kVehiclesBackup = "vehicles.db.pzcleaner-bak";
const std::set<std::string> kCellKinds = {"zpop", "apop", "chunkdata"};

class Reporter {
public:
    Reporter(const App::Progress& progress, std::atomic<std::int64_t>& done, std::int64_t total)
        : progress_(progress), done_(done), total_(total), thread_([this] { run(); }) {}

    ~Reporter() {
        {
            std::lock_guard lock(mutex_);
            stop_ = true;
        }
        wake_.notify_all();
        thread_.join();
        progress_(done_.load(), total_);
    }

private:
    void run() {
        std::unique_lock lock(mutex_);
        while (!wake_.wait_for(lock, std::chrono::milliseconds(200), [this] { return stop_; })) {
            progress_(done_.load(), total_);
        }
    }

    const App::Progress& progress_;
    std::atomic<std::int64_t>& done_;
    std::int64_t total_;
    std::mutex mutex_;
    std::condition_variable wake_;
    bool stop_ = false;
    std::thread thread_;
};

bool parseInt(std::string_view text, int& value) {
    if (text.empty() || text.size() > 11) {
        return false;
    }
    std::size_t i = text[0] == '-' ? 1 : 0;
    if (i == text.size()) {
        return false;
    }
    long long n = 0;
    for (; i < text.size(); ++i) {
        if (text[i] < '0' || text[i] > '9') {
            return false;
        }
        n = n * 10 + (text[i] - '0');
    }
    value = static_cast<int>(text[0] == '-' ? -n : n);
    return true;
}

bool parseChunkName(const std::string& name, int& y) {
    return name.size() > 4 && name.compare(name.size() - 4, 4, ".bin") == 0 &&
           parseInt(std::string_view(name).substr(0, name.size() - 4), y);
}

std::string backupDir(const std::string& root) {
    std::string saves = parentPath(parentPath(root));
    if (baseName(saves) == "Saves") {
        return joinPath(joinPath(parentPath(saves), kBackupFolder), baseName(root));
    }
    return joinPath(joinPath(parentPath(root), kBackupFolder), baseName(root));
}

std::string trimRoot(std::string path) {
    while (path.size() > 1 && path.back() == '/') {
        path.pop_back();
    }
    return path;
}

std::string stampNow() {
    std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    std::ostringstream out;
    out << std::put_time(&local, "%Y-%m-%d_%H-%M-%S");
    return out.str();
}

std::int64_t chunkKey(int x, int y) {
    return (static_cast<std::int64_t>(x) << 32) ^ static_cast<std::uint32_t>(y);
}

class Database {
public:
    explicit Database(const std::string& bytes) {
        sqlite3_open(":memory:", &db_);
        auto* buffer = static_cast<unsigned char*>(sqlite3_malloc64(bytes.size()));
        std::copy(bytes.begin(), bytes.end(), buffer);
        int rc = sqlite3_deserialize(db_, "main", buffer, static_cast<sqlite3_int64>(bytes.size()),
                                     static_cast<sqlite3_int64>(bytes.size()),
                                     SQLITE_DESERIALIZE_FREEONCLOSE | SQLITE_DESERIALIZE_RESIZEABLE);
        if (rc != SQLITE_OK) {
            throw std::runtime_error("vehicles.db could not be opened");
        }
    }
    ~Database() { sqlite3_close(db_); }

    void exec(const char* sql) {
        if (sqlite3_exec(db_, sql, nullptr, nullptr, nullptr) != SQLITE_OK) {
            throw std::runtime_error(std::string("vehicles.db: ") + sqlite3_errmsg(db_));
        }
    }

    sqlite3_stmt* prepare(const char* sql) {
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &statement, nullptr) != SQLITE_OK) {
            throw std::runtime_error(std::string("vehicles.db: ") + sqlite3_errmsg(db_));
        }
        return statement;
    }

    std::string bytes() {
        sqlite3_int64 size = 0;
        unsigned char* data = sqlite3_serialize(db_, "main", &size, 0);
        if (data == nullptr) {
            throw std::runtime_error("vehicles.db could not be saved");
        }
        std::string out(reinterpret_cast<char*>(data), static_cast<std::size_t>(size));
        sqlite3_free(data);
        return out;
    }

private:
    sqlite3* db_ = nullptr;
};

std::vector<std::int64_t> editVehicles(std::string& bytes, const std::unordered_set<std::int64_t>& doomed) {
    Database db(bytes);
    sqlite3_stmt* table = db.prepare("SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'vehicles'");
    bool hasTable = sqlite3_step(table) == SQLITE_ROW;
    sqlite3_finalize(table);
    if (!hasTable) {
        return {};
    }
    std::vector<std::int64_t> ids;
    sqlite3_stmt* rows = db.prepare("SELECT id, wx, wy FROM vehicles");
    while (sqlite3_step(rows) == SQLITE_ROW) {
        if (doomed.count(chunkKey(sqlite3_column_int(rows, 1), sqlite3_column_int(rows, 2))) > 0) {
            ids.push_back(sqlite3_column_int64(rows, 0));
        }
    }
    sqlite3_finalize(rows);
    if (ids.empty()) {
        return ids;
    }
    db.exec("BEGIN");
    sqlite3_stmt* remove = db.prepare("DELETE FROM vehicles WHERE id = ?");
    for (std::int64_t id : ids) {
        sqlite3_bind_int64(remove, 1, id);
        if (sqlite3_step(remove) != SQLITE_DONE) {
            sqlite3_finalize(remove);
            throw std::runtime_error("vehicles.db: a vehicle row could not be removed");
        }
        sqlite3_reset(remove);
    }
    sqlite3_finalize(remove);
    db.exec("COMMIT");
    sqlite3_stmt* check = db.prepare("PRAGMA integrity_check");
    bool ok = sqlite3_step(check) == SQLITE_ROW &&
              std::string(reinterpret_cast<const char*>(sqlite3_column_text(check, 0))) == "ok";
    sqlite3_finalize(check);
    if (!ok) {
        throw std::runtime_error("vehicles.db failed its integrity check after the edit, so it was left as it was");
    }
    bytes = db.bytes();
    return ids;
}

struct Job {
    Store& store;
    std::string root;
    std::string batch;
    std::atomic<std::int64_t> done{0};

    std::string path(std::initializer_list<std::string> parts, const std::string& base) const {
        std::string out = base;
        for (const std::string& part : parts) {
            out = joinPath(out, part);
        }
        return out;
    }

    bool discard(std::initializer_list<std::string> parts) {
        try {
            if (batch.empty()) {
                store.remove(path(parts, root));
            } else {
                store.rename(path(parts, root), path(parts, batch));
            }
            return true;
        } catch (const FsError& error) {
            if (error.notFound) {
                return false;
            }
            throw;
        }
    }
};

std::vector<std::int64_t> deleteVehicles(Job& job, const std::vector<std::pair<int, int>>& chunks) {
    try {
        if (job.store.stat(joinPath(job.root, "vehicles.db-journal")).size > 0) {
            throw std::runtime_error("vehicles.db has an unfinished write journal next to it. Start and stop the game "
                                     "once so it settles, then try again");
        }
    } catch (const FsError& error) {
        if (!error.notFound) {
            throw;
        }
    }
    std::string bytes;
    try {
        bytes = job.store.readFile(joinPath(job.root, "vehicles.db"));
    } catch (const FsError& error) {
        if (error.notFound) {
            return {};
        }
        throw;
    }
    std::unordered_set<std::int64_t> doomed;
    for (const auto& [x, y] : chunks) {
        doomed.insert(chunkKey(x, y));
    }
    std::vector<std::int64_t> ids = editVehicles(bytes, doomed);
    if (ids.empty()) {
        return ids;
    }
    std::string fresh = joinPath(job.root, "vehicles.db.pzcleaner-new");
    job.store.writeFile(fresh, bytes);
    std::string backup = job.batch.empty() ? joinPath(job.root, kVehiclesBackup) : joinPath(job.batch, "vehicles.db");
    job.store.replace(joinPath(job.root, "vehicles.db"), backup);
    job.store.rename(fresh, joinPath(job.root, "vehicles.db"));
    return ids;
}

void deleteChunks(Job& job, const std::vector<std::pair<int, int>>& chunks) {
    std::vector<int> columns;
    std::unordered_set<int> seen;
    for (const auto& chunk : chunks) {
        if (seen.insert(chunk.first).second) {
            columns.push_back(chunk.first);
        }
    }
    if (!job.batch.empty()) {
        std::function<void(const int&)> makeColumn = [&](const int& x) {
            job.store.mkdirAll(job.path({"map", std::to_string(x)}, job.batch));
        };
        runPool(columns, kFileWorkers, makeColumn);
    }
    std::function<void(const std::pair<int, int>&)> discardChunk = [&](const std::pair<int, int>& chunk) {
        job.discard({"map", std::to_string(chunk.first), std::to_string(chunk.second) + ".bin"});
        ++job.done;
    };
    runPool(chunks, kFileWorkers, discardChunk);
    std::function<void(const int&)> removeColumn = [&](const int& x) {
        try {
            job.store.remove(job.path({"map", std::to_string(x)}, job.root));
        } catch (const std::exception&) {
        }
    };
    runPool(columns, kFileWorkers, removeColumn);
}

std::int64_t deleteCellFiles(Job& job, const std::vector<std::pair<int, int>>& cells,
                             const std::vector<std::string>& kinds) {
    std::atomic<std::int64_t> removed{0};
    for (const std::string& kind : kinds) {
        if (!job.batch.empty()) {
            job.store.mkdirAll(joinPath(job.batch, kind));
        }
        std::function<void(const std::pair<int, int>&)> discardCell = [&](const std::pair<int, int>& cell) {
            std::string name = kind + "_" + std::to_string(cell.first) + "_" + std::to_string(cell.second) + ".bin";
            if (job.discard({kind, name})) {
                ++removed;
            }
            ++job.done;
        };
        runPool(cells, kFileWorkers, discardCell);
    }
    return removed.load();
}

std::vector<std::pair<int, int>> pairs(const json& list) {
    std::vector<std::pair<int, int>> out;
    out.reserve(list.size());
    for (const auto& item : list) {
        out.emplace_back(item.at(0).get<int>(), item.at(1).get<int>());
    }
    return out;
}

}

json App::open(std::shared_ptr<Store> store, const std::string& root) {
    std::lock_guard lock(mutex_);
    session_ = {std::move(store), root, backupDir(root)};
    return {{"name", baseName(root)}};
}

json App::connect(const json& request) {
    std::string host = request.value("host", "");
    std::string user = request.value("user", "");
    std::string password = request.value("password", "");
    std::string root = trimRoot(request.value("path", ""));
    int port = request.value("port", 0);
    if (host.empty() || user.empty() || password.empty() || root.empty()) {
        throw std::runtime_error("fill in the host, the user, the password and the save folder path");
    }
    if (port == 0) {
        port = 22;
    }
    sftp::UnknownHost unknown;
    std::unique_ptr<sftp::Link> link =
        sftp::Link::open(host, port, user, password, request.value("trust", ""), knownHosts_, unknown);
    if (!link) {
        return {{"fingerprint", unknown.fingerprint}, {"keyType", unknown.keyType}};
    }
    auto store = std::make_shared<RemoteStore>(std::move(link));
    try {
        if (!store->stat(root).dir) {
            throw std::runtime_error("that path is a file, not a folder");
        }
    } catch (const std::exception& error) {
        throw std::runtime_error("could not open the save folder " + root + ": " + error.what());
    }
    return open(store, root);
}

json App::openLocal(const std::string& dir) {
    std::string root = dir;
    for (char& c : root) {
        if (c == '\\') {
            c = '/';
        }
    }
    root = trimRoot(root);
    auto store = std::make_shared<LocalStore>();
    if (!store->stat(root).dir) {
        throw std::runtime_error("that path is a file, not a folder");
    }
    return open(store, root);
}

void App::disconnect() {
    std::shared_ptr<Store> old;
    {
        std::lock_guard lock(mutex_);
        old = std::move(session_.store);
        session_ = {};
    }
}

App::Session App::current() {
    std::lock_guard lock(mutex_);
    if (!session_.store) {
        throw NamedError("NotConnectedError", "no save is open");
    }
    return session_;
}

std::pair<std::shared_ptr<Store>, std::string> App::resolve(const std::string& rel) {
    Session session = current();
    if (rel.empty()) {
        return {session.store, session.root};
    }
    std::vector<std::string> parts;
    std::size_t start = 0;
    for (;;) {
        std::size_t slash = rel.find('/', start);
        std::string part = rel.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (part.empty() || part == "." || part == ".." || part.find('\\') != std::string::npos) {
            throw NamedError("TypeError", "bad path " + rel);
        }
        parts.push_back(part);
        if (slash == std::string::npos) {
            break;
        }
        start = slash + 1;
    }
    std::string full = parts[0] == kTrashDir ? session.backup : joinPath(session.root, parts[0]);
    for (std::size_t i = 1; i < parts.size(); ++i) {
        full = joinPath(full, parts[i]);
    }
    return {session.store, full};
}

json App::list(const std::string& rel) {
    auto [store, full] = resolve(rel);
    json out = json::array();
    for (const Entry& entry : store->readDir(full)) {
        out.push_back({{"name", entry.name}, {"dir", entry.dir}, {"size", entry.size}});
    }
    return out;
}

json App::stat(const std::string& rel) {
    auto [store, full] = resolve(rel);
    Entry entry = store->stat(full);
    return {{"name", entry.name}, {"dir", entry.dir}, {"size", entry.size}};
}

std::string App::read(const std::string& rel) {
    auto [store, full] = resolve(rel);
    return base64Encode(store->readFile(full));
}

void App::write(const std::string& rel, const std::string& base64) {
    auto [store, full] = resolve(rel);
    store->writeFile(full, base64Decode(base64));
}

void App::mkdir(const std::string& rel) {
    auto [store, full] = resolve(rel);
    store->mkdirAll(full);
}

void App::rename(const std::string& from, const std::string& to) {
    auto [store, fullFrom] = resolve(from);
    auto fullTo = resolve(to).second;
    store->rename(fullFrom, fullTo);
}

void App::remove(const std::string& rel, bool recursive) {
    auto [store, full] = resolve(rel);
    Entry entry = store->stat(full);
    if (entry.dir && recursive) {
        removeTree(*store, full);
    }
    try {
        store->remove(full);
    } catch (const FsError& error) {
        if (entry.dir && !recursive && !store->readDir(full).empty()) {
            throw NamedError("InvalidModificationError", rel + " is not empty");
        }
        throw;
    }
}

json App::index(const std::string& rel) {
    auto [store, full] = resolve(rel);
    std::vector<int> columns;
    for (const Entry& entry : store->readDir(full)) {
        int x = 0;
        if (entry.dir && parseInt(entry.name, x)) {
            columns.push_back(x);
        }
    }
    std::mutex mutex;
    std::vector<int> flat;
    std::atomic<std::int64_t> done{0};
    {
        Reporter reporter(progress_, done, static_cast<std::int64_t>(columns.size()));
        std::function<void(const int&)> listColumn = [&](const int& x) {
            std::vector<Entry> names = store->readDir(joinPath(full, std::to_string(x)));
            std::vector<int> found;
            found.reserve(names.size() * 2);
            for (const Entry& entry : names) {
                int y = 0;
                if (parseChunkName(entry.name, y)) {
                    found.push_back(x);
                    found.push_back(y);
                }
            }
            std::lock_guard lock(mutex);
            flat.insert(flat.end(), found.begin(), found.end());
            ++done;
        };
        runPool(columns, kListWorkers, listColumn);
    }
    return flat;
}

json App::execute(const json& request) {
    Session session = current();
    std::vector<std::pair<int, int>> chunks = pairs(request.at("chunks"));
    std::vector<std::pair<int, int>> cells = pairs(request.at("cells"));
    std::vector<std::string> kinds = request.at("cellKinds").get<std::vector<std::string>>();
    for (const std::string& kind : kinds) {
        if (kCellKinds.count(kind) == 0) {
            throw std::runtime_error("unknown cell file kind " + kind);
        }
    }
    Job job{*session.store, session.root, ""};
    json result = {{"cellFiles", 0}, {"vehicleIds", json::array()}, {"stamp", ""}};
    if (request.value("trash", true)) {
        std::string stamp = stampNow();
        result["stamp"] = stamp;
        job.batch = joinPath(session.backup, stamp);
        job.store.mkdirAll(job.batch);
    }
    Reporter reporter(progress_, job.done,
                      static_cast<std::int64_t>(chunks.size() + cells.size() * kinds.size()));
    if (request.value("vehicles", false)) {
        result["vehicleIds"] = deleteVehicles(job, chunks);
    }
    deleteChunks(job, chunks);
    result["cellFiles"] = deleteCellFiles(job, cells, kinds);
    return result;
}

std::int64_t App::restore(const std::string& stamp) {
    auto [store, batch] = resolve(kTrashDir + "/" + stamp);
    Session session = current();
    Tree tree = walk(*store, batch);
    for (std::size_t i = 1; i < tree.levels.size(); ++i) {
        std::function<void(const std::string&)> makeDir = [&](const std::string& dir) {
            store->mkdirAll(session.root + dir.substr(batch.size()));
        };
        runPool(tree.levels[i], kFileWorkers, makeDir);
    }
    std::atomic<std::int64_t> done{0};
    {
        Reporter reporter(progress_, done, static_cast<std::int64_t>(tree.files.size()));
        std::function<void(const std::string&)> putBack = [&](const std::string& file) {
            store->replace(file, session.root + file.substr(batch.size()));
            ++done;
        };
        runPool(tree.files, kFileWorkers, putBack);
    }
    removeTree(*store, batch);
    store->remove(batch);
    try {
        if (store->readDir(session.backup).empty()) {
            store->remove(session.backup);
        }
    } catch (const std::exception&) {
    }
    return static_cast<std::int64_t>(tree.files.size());
}
