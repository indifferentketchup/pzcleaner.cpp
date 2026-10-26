#pragma once

#include "app.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

constexpr int kChunkSquares = 8;
constexpr int kCellChunks = 32;
constexpr int kMapW = 78 * 256;
constexpr int kMapH = 63 * 256;

struct ChunkRect {
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;
};

int floorDiv(int value, int divisor);
bool rectContains(const ChunkRect& rect, int x, int y);
ChunkRect chunkRectBetween(double ax, double ay, double bx, double by);
ChunkRect squaresToChunkRect(double x, double y, double w, double h, int padChunks);

struct Safehouse {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    std::string owner;
    std::vector<std::string> players;
    std::string title;
};

struct Player {
    double x = 0;
    double y = 0;
};

struct Vehicle {
    std::int64_t id = 0;
    int wx = 0;
    int wy = 0;
};

template <typename T>
struct Listing {
    std::vector<T> list;
    std::string error;
};

struct Save {
    std::string name;
    Pairs chunks;
    std::optional<ChunkRect> bounds;
    Listing<Safehouse> safehouses;
    Listing<Player> players;
    Listing<Vehicle> vehicles;
    std::vector<std::string> trash;

    void dropChunks(const std::vector<std::uint8_t>& doomed);
    void dropVehicles(const std::vector<std::int64_t>& ids);
};

Save openSave(App& app, const std::string& name);

struct Selection {
    std::vector<ChunkRect> rects;
    bool inverted = false;
    std::vector<ChunkRect> protectedRects;
};

struct Plan {
    std::vector<std::uint8_t> doomed;
    std::size_t chunkCount = 0;
    Pairs clearedCells;
    std::vector<std::string> cellKinds;
    std::size_t vehicleCount = 0;
};

Plan buildPlan(const Save& save, const Selection& selection, std::vector<std::string> cellKinds, bool vehicles);
