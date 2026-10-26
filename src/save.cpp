#include "save.hpp"

#include "database.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace {

constexpr int kMinVersion = 194;

class MetaReader {
public:
    explicit MetaReader(const std::string& data) : data_(data) {}

    std::int32_t int32() {
        need(4);
        std::uint32_t value = (byte(0) << 24) | (byte(1) << 16) | (byte(2) << 8) | byte(3);
        pos_ += 4;
        return static_cast<std::int32_t>(value);
    }

    void skip(std::int64_t count) {
        if (count < 0) {
            throw std::runtime_error("map_meta.bin has a bad length in it");
        }
        need(static_cast<std::uint64_t>(count));
        pos_ += static_cast<std::size_t>(count);
    }

    std::string string() {
        need(2);
        std::size_t length = (byte(0) << 8) | byte(1);
        pos_ += 2;
        need(length);
        std::string out = data_.substr(pos_, length);
        pos_ += length;
        return out;
    }

    std::vector<std::string> strings() {
        std::int32_t count = int32();
        std::vector<std::string> out;
        for (std::int32_t i = 0; i < count; ++i) {
            out.push_back(string());
        }
        return out;
    }

private:
    std::uint32_t byte(std::size_t offset) const { return static_cast<unsigned char>(data_[pos_ + offset]); }

    void need(std::uint64_t count) const {
        if (count > data_.size() - pos_) {
            throw std::runtime_error("map_meta.bin ended early");
        }
    }

    const std::string& data_;
    std::size_t pos_ = 0;
};

void skipCells(MetaReader& reader, int version) {
    std::int64_t minX = reader.int32();
    std::int64_t minY = reader.int32();
    std::int64_t maxX = reader.int32();
    std::int64_t maxY = reader.int32();
    std::int64_t cells = (maxX - minX + 1) * (maxY - minY + 1);
    std::int64_t buildingBytes = version >= 201 ? 23 : 19;
    for (std::int64_t i = 0; i < cells; ++i) {
        reader.skip(std::int64_t{reader.int32()} * 10);
        reader.skip(std::int64_t{reader.int32()} * buildingBytes);
    }
}

Safehouse readSafehouse(MetaReader& reader, int version) {
    Safehouse house;
    house.x = reader.int32();
    house.y = reader.int32();
    house.w = reader.int32();
    house.h = reader.int32();
    house.owner = reader.string();
    if (version >= 216) {
        reader.skip(4);
    }
    house.players = reader.strings();
    reader.skip(8);
    house.title = reader.string();
    if (version >= 223) {
        reader.skip(8);
        reader.string();
    }
    reader.strings();
    return house;
}

std::vector<Safehouse> parseSafehouses(const std::string& data) {
    if (data.size() < 4 || data.compare(0, 4, "META") != 0) {
        throw std::runtime_error("map_meta.bin does not start with its meta header");
    }
    MetaReader reader(data);
    reader.skip(4);
    int version = reader.int32();
    if (version < kMinVersion) {
        throw std::runtime_error("map_meta.bin is world version " + std::to_string(version) +
                                 ", older than this tool reads");
    }
    skipCells(reader, version);
    std::int32_t count = reader.int32();
    std::vector<Safehouse> safehouses;
    for (std::int32_t i = 0; i < count; ++i) {
        safehouses.push_back(readSafehouse(reader, version));
    }
    return safehouses;
}

bool isLiving(sqlite3_stmt* row, int column) {
    switch (sqlite3_column_type(row, column)) {
    case SQLITE_NULL:
        return true;
    case SQLITE_INTEGER:
        return sqlite3_column_int64(row, column) == 0;
    case SQLITE_FLOAT:
        return sqlite3_column_double(row, column) == 0.0;
    case SQLITE_TEXT:
        return sqlite3_column_bytes(row, column) == 0;
    default:
        return false;
    }
}

std::vector<Player> readLivingPlayers(App& app) {
    std::optional<std::string> bytes = app.readFile("players.db");
    if (!bytes) {
        return {};
    }
    Database db("players.db", *bytes);
    std::vector<Player> players;
    for (const char* table : {"networkPlayers", "localPlayers"}) {
        if (!db.hasTable(table)) {
            continue;
        }
        std::string query = std::string("SELECT x, y, isDead FROM ") + table;
        Statement rows = db.prepare(query.c_str());
        while (rows.step()) {
            if (isLiving(rows.get(), 2)) {
                players.push_back({sqlite3_column_double(rows.get(), 0), sqlite3_column_double(rows.get(), 1)});
            }
        }
    }
    return players;
}

std::vector<Vehicle> readVehicles(App& app) {
    std::optional<std::string> bytes = app.readFile("vehicles.db");
    if (!bytes) {
        return {};
    }
    Database db("vehicles.db", *bytes);
    if (!db.hasTable("vehicles")) {
        return {};
    }
    std::vector<Vehicle> vehicles;
    Statement rows = db.prepare("SELECT id, wx, wy FROM vehicles");
    while (rows.step()) {
        vehicles.push_back({sqlite3_column_int64(rows.get(), 0), sqlite3_column_int(rows.get(), 1),
                            sqlite3_column_int(rows.get(), 2)});
    }
    return vehicles;
}

std::vector<Safehouse> readSafehouses(App& app) {
    std::optional<std::string> bytes = app.readFile("map_meta.bin");
    if (!bytes) {
        throw std::runtime_error("map_meta.bin is missing");
    }
    return parseSafehouses(*bytes);
}

template <typename T>
Listing<T> listOrError(const std::function<std::vector<T>(App&)>& read, App& app) {
    try {
        return {read(app), ""};
    } catch (const std::exception& error) {
        return {{}, error.what()};
    }
}

std::optional<ChunkRect> boundsOf(const Pairs& chunks) {
    if (chunks.empty()) {
        return std::nullopt;
    }
    ChunkRect bounds{chunks[0].first, chunks[0].second, chunks[0].first, chunks[0].second};
    for (const auto& [x, y] : chunks) {
        bounds.x0 = std::min(bounds.x0, x);
        bounds.x1 = std::max(bounds.x1, x);
        bounds.y0 = std::min(bounds.y0, y);
        bounds.y1 = std::max(bounds.y1, y);
    }
    bounds.x1 += 1;
    bounds.y1 += 1;
    return bounds;
}

bool isSelected(const Selection& selection, int x, int y) {
    for (const ChunkRect& rect : selection.protectedRects) {
        if (rectContains(rect, x, y)) {
            return false;
        }
    }
    bool inside = std::any_of(selection.rects.begin(), selection.rects.end(),
                              [x, y](const ChunkRect& rect) { return rectContains(rect, x, y); });
    return selection.inverted ? !inside : inside;
}

class SelectionMask {
public:
    SelectionMask(const Selection& selection, const ChunkRect& bounds) : selection_(selection), bounds_(bounds) {
        std::int64_t width = bounds.x1 - bounds.x0;
        std::int64_t height = bounds.y1 - bounds.y0;
        if (width * height > kMaxCells) {
            return;
        }
        width_ = static_cast<std::size_t>(width);
        mask_.assign(static_cast<std::size_t>(width * height), 0);
        for (const ChunkRect& rect : selection.rects) {
            fill(rect, kInside);
        }
        for (const ChunkRect& rect : selection.protectedRects) {
            fill(rect, kProtected);
        }
    }

    bool selected(int x, int y) const {
        if (mask_.empty() || !rectContains(bounds_, x, y)) {
            return isSelected(selection_, x, y);
        }
        std::uint8_t bits = mask_[index(x, y)];
        if (bits & kProtected) {
            return false;
        }
        bool inside = (bits & kInside) != 0;
        return selection_.inverted ? !inside : inside;
    }

private:
    static constexpr std::int64_t kMaxCells = std::int64_t{1} << 26;
    static constexpr std::uint8_t kInside = 1;
    static constexpr std::uint8_t kProtected = 2;

    std::size_t index(int x, int y) const {
        return static_cast<std::size_t>(y - bounds_.y0) * width_ + static_cast<std::size_t>(x - bounds_.x0);
    }

    void fill(const ChunkRect& rect, std::uint8_t bit) {
        int x0 = std::max(rect.x0, bounds_.x0);
        int x1 = std::min(rect.x1, bounds_.x1);
        int y0 = std::max(rect.y0, bounds_.y0);
        int y1 = std::min(rect.y1, bounds_.y1);
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                mask_[index(x, y)] |= bit;
            }
        }
    }

    const Selection& selection_;
    ChunkRect bounds_;
    std::size_t width_ = 0;
    std::vector<std::uint8_t> mask_;
};

}

int floorDiv(int value, int divisor) {
    int quotient = value / divisor;
    if (value % divisor != 0 && (value < 0) != (divisor < 0)) {
        --quotient;
    }
    return quotient;
}

bool rectContains(const ChunkRect& rect, int x, int y) {
    return x >= rect.x0 && x < rect.x1 && y >= rect.y0 && y < rect.y1;
}

ChunkRect chunkRectBetween(double ax, double ay, double bx, double by) {
    return {
        static_cast<int>(std::floor(std::min(ax, bx) / kChunkSquares)),
        static_cast<int>(std::floor(std::min(ay, by) / kChunkSquares)),
        static_cast<int>(std::floor(std::max(ax, bx) / kChunkSquares)) + 1,
        static_cast<int>(std::floor(std::max(ay, by) / kChunkSquares)) + 1,
    };
}

ChunkRect squaresToChunkRect(double x, double y, double w, double h, int padChunks) {
    return {
        static_cast<int>(std::floor(x / kChunkSquares)) - padChunks,
        static_cast<int>(std::floor(y / kChunkSquares)) - padChunks,
        static_cast<int>(std::ceil((x + w) / kChunkSquares)) + padChunks,
        static_cast<int>(std::ceil((y + h) / kChunkSquares)) + padChunks,
    };
}

void Save::dropChunks(const std::vector<std::uint8_t>& doomed) {
    std::size_t kept = 0;
    for (std::size_t i = 0; i < chunks.size(); ++i) {
        if (!doomed[i]) {
            chunks[kept++] = chunks[i];
        }
    }
    chunks.resize(kept);
    bounds = boundsOf(chunks);
}

void Save::dropVehicles(const std::vector<std::int64_t>& ids) {
    std::unordered_set<std::int64_t> gone(ids.begin(), ids.end());
    std::erase_if(vehicles.list, [&gone](const Vehicle& vehicle) { return gone.count(vehicle.id) > 0; });
}

Save openSave(App& app, const std::string& name) {
    Save save;
    save.name = name;
    save.chunks = app.index();
    save.trash = app.listTrash();
    if (save.chunks.empty() && save.trash.empty()) {
        throw std::runtime_error("No Build 42 chunk files in that folder. Pick the save folder itself, the one "
                                 "holding map_meta.bin and a map folder.");
    }
    std::sort(save.chunks.begin(), save.chunks.end());
    save.bounds = boundsOf(save.chunks);
    save.safehouses = listOrError<Safehouse>(readSafehouses, app);
    save.players = listOrError<Player>(readLivingPlayers, app);
    save.vehicles = listOrError<Vehicle>(readVehicles, app);
    return save;
}

Plan buildPlan(const Save& save, const Selection& selection, std::vector<std::string> cellKinds, bool vehicles) {
    Plan plan;
    plan.cellKinds = std::move(cellKinds);
    plan.doomed.assign(save.chunks.size(), 0);
    if (selection.rects.empty() || !save.bounds) {
        return plan;
    }
    constexpr std::uint8_t kTouched = 1;
    constexpr std::uint8_t kKept = 2;
    std::unordered_map<std::int64_t, std::uint8_t> cells;
    std::int64_t lastCell = 0;
    std::uint8_t* lastBits = nullptr;
    SelectionMask mask(selection, *save.bounds);
    for (std::size_t i = 0; i < save.chunks.size(); ++i) {
        auto [x, y] = save.chunks[i];
        std::int64_t cell = chunkKey(floorDiv(x, kCellChunks), floorDiv(y, kCellChunks));
        if (lastBits == nullptr || cell != lastCell) {
            lastBits = &cells[cell];
            lastCell = cell;
        }
        if (mask.selected(x, y)) {
            plan.doomed[i] = 1;
            ++plan.chunkCount;
            *lastBits |= kTouched;
        } else {
            *lastBits |= kKept;
        }
    }
    for (const auto& [cell, bits] : cells) {
        if (bits == kTouched) {
            plan.clearedCells.emplace_back(static_cast<int>(cell >> 32), static_cast<std::int32_t>(cell & 0xFFFFFFFF));
        }
    }
    std::sort(plan.clearedCells.begin(), plan.clearedCells.end());
    if (vehicles) {
        for (const Vehicle& vehicle : save.vehicles.list) {
            if (std::binary_search(save.chunks.begin(), save.chunks.end(), std::pair{vehicle.wx, vehicle.wy}) &&
                mask.selected(vehicle.wx, vehicle.wy)) {
                ++plan.vehicleCount;
            }
        }
    }
    return plan;
}
