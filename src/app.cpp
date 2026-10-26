#include "app.hpp"

#include "database.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <iomanip>
#include <set>
#include <sstream>
#include <thread>
#include <unordered_set>

namespace {

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

bool hasFolder(Store& store, const std::string& path) {
    try {
        return store.stat(path).dir;
    } catch (const FsError& error) {
        if (error.notFound) {
            return false;
        }
        throw;
    }
}

std::vector<std::int64_t> editVehicles(std::string& bytes, const std::unordered_set<std::int64_t>& doomed) {
    Database db("vehicles.db", bytes);
    if (!db.hasTable("vehicles")) {
        return {};
    }
    std::vector<std::int64_t> ids;
    {
        Statement rows = db.prepare("SELECT id, wx, wy FROM vehicles");
        while (rows.step()) {
            if (doomed.count(chunkKey(sqlite3_column_int(rows.get(), 1), sqlite3_column_int(rows.get(), 2))) > 0) {
                ids.push_back(sqlite3_column_int64(rows.get(), 0));
            }
        }
    }
    if (ids.empty()) {
        return ids;
    }
    db.exec("BEGIN");
    {
        Statement remove = db.prepare("DELETE FROM vehicles WHERE id = ?");
        for (std::int64_t id : ids) {
            sqlite3_bind_int64(remove.get(), 1, id);
            remove.step();
            remove.reset();
        }
    }
    db.exec("COMMIT");
    Statement check = db.prepare("PRAGMA integrity_check");
    bool ok = check.step() && std::string(reinterpret_cast<const char*>(sqlite3_column_text(check.get(), 0))) == "ok";
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

std::vector<std::int64_t> deleteVehicles(Job& job, const Pairs& chunks) {
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

void deleteChunks(Job& job, const Pairs& chunks) {
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

std::int64_t deleteCellFiles(Job& job, const Pairs& cells, const std::vector<std::string>& kinds) {
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

}

std::string App::open(std::shared_ptr<Store> store, const std::string& root) {
    std::lock_guard lock(mutex_);
    session_ = {std::move(store), root, backupDir(root)};
    return baseName(root);
}

ConnectResult App::connect(const ConnectRequest& request) {
    std::string root = trimRoot(request.path);
    if (request.host.empty() || request.user.empty() || request.password.empty() || root.empty()) {
        throw std::runtime_error("fill in the host, the user, the password and the save folder path");
    }
    int port = request.port == 0 ? 22 : request.port;
    sftp::UnknownHost unknown;
    std::unique_ptr<sftp::Link> link =
        sftp::Link::open(request.host, port, request.user, request.password, request.trust, knownHosts_, unknown);
    if (!link) {
        return {"", unknown.fingerprint, unknown.keyType};
    }
    auto store = std::make_shared<RemoteStore>(std::move(link));
    try {
        if (!store->stat(root).dir) {
            throw std::runtime_error("that path is a file, not a folder");
        }
    } catch (const std::exception& error) {
        throw std::runtime_error("could not open the save folder " + root + ": " + error.what());
    }
    return {open(store, root), "", ""};
}

std::string App::openLocal(const std::string& dir) {
    std::string root = dir;
    for (char& c : root) {
        if (c == '\\') {
            c = '/';
        }
    }
    root = trimRoot(root);
    if (root.empty()) {
        throw std::runtime_error("fill in the folder path");
    }
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
        throw std::runtime_error("no save is open");
    }
    return session_;
}

Pairs App::index() {
    Session session = current();
    std::string full = joinPath(session.root, "map");
    if (!hasFolder(*session.store, full)) {
        return {};
    }
    std::vector<int> columns;
    for (const Entry& entry : session.store->readDir(full)) {
        int x = 0;
        if (entry.dir && parseInt(entry.name, x)) {
            columns.push_back(x);
        }
    }
    std::mutex mutex;
    Pairs chunks;
    std::atomic<std::int64_t> done{0};
    {
        Reporter reporter(progress_, done, static_cast<std::int64_t>(columns.size()));
        std::function<void(const int&)> listColumn = [&](const int& x) {
            std::vector<Entry> names = session.store->readDir(joinPath(full, std::to_string(x)));
            Pairs found;
            found.reserve(names.size());
            for (const Entry& entry : names) {
                int y = 0;
                if (parseChunkName(entry.name, y)) {
                    found.emplace_back(x, y);
                }
            }
            std::lock_guard lock(mutex);
            chunks.insert(chunks.end(), found.begin(), found.end());
            ++done;
        };
        runPool(columns, kListWorkers, listColumn);
    }
    return chunks;
}

std::optional<std::string> App::readFile(const std::string& name) {
    Session session = current();
    try {
        return session.store->readFile(joinPath(session.root, name));
    } catch (const FsError& error) {
        if (error.notFound) {
            return std::nullopt;
        }
        throw;
    }
}

std::vector<std::string> App::listTrash() {
    Session session = current();
    if (!hasFolder(*session.store, session.backup)) {
        return {};
    }
    std::vector<std::string> stamps;
    for (const Entry& entry : session.store->readDir(session.backup)) {
        if (entry.dir) {
            stamps.push_back(entry.name);
        }
    }
    std::sort(stamps.begin(), stamps.end());
    return stamps;
}

ExecuteResult App::execute(const ExecuteRequest& request) {
    Session session = current();
    for (const std::string& kind : request.cellKinds) {
        if (kCellKinds.count(kind) == 0) {
            throw std::runtime_error("unknown cell file kind " + kind);
        }
    }
    Job job{*session.store, session.root, ""};
    ExecuteResult result;
    if (request.trash) {
        result.stamp = stampNow();
        job.batch = joinPath(session.backup, result.stamp);
        job.store.mkdirAll(job.batch);
    }
    Reporter reporter(progress_, job.done,
                      static_cast<std::int64_t>(request.chunks.size() + request.cells.size() * request.cellKinds.size()));
    if (request.vehicles) {
        result.vehicleIds = deleteVehicles(job, request.chunks);
    }
    deleteChunks(job, request.chunks);
    result.cellFiles = deleteCellFiles(job, request.cells, request.cellKinds);
    return result;
}

std::int64_t App::restore(const std::string& stamp) {
    Session session = current();
    if (stamp.empty() || stamp.find('/') != std::string::npos || stamp.find('\\') != std::string::npos ||
        stamp == "." || stamp == "..") {
        throw std::runtime_error("bad trash batch name " + stamp);
    }
    Store& store = *session.store;
    std::string batch = joinPath(session.backup, stamp);
    Tree tree = walk(store, batch);
    for (std::size_t i = 1; i < tree.levels.size(); ++i) {
        std::function<void(const std::string&)> makeDir = [&](const std::string& dir) {
            store.mkdirAll(session.root + dir.substr(batch.size()));
        };
        runPool(tree.levels[i], kFileWorkers, makeDir);
    }
    std::atomic<std::int64_t> done{0};
    {
        Reporter reporter(progress_, done, static_cast<std::int64_t>(tree.files.size()));
        std::function<void(const std::string&)> putBack = [&](const std::string& file) {
            store.replace(file, session.root + file.substr(batch.size()));
            ++done;
        };
        runPool(tree.files, kFileWorkers, putBack);
    }
    removeTree(store, batch);
    store.remove(batch);
    try {
        if (store.readDir(session.backup).empty()) {
            store.remove(session.backup);
        }
    } catch (const std::exception&) {
    }
    return static_cast<std::int64_t>(tree.files.size());
}

void App::emptyTrash() {
    Session session = current();
    removeTree(*session.store, session.backup);
    session.store->remove(session.backup);
}
