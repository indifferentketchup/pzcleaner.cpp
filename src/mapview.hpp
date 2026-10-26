#pragma once

#include "save.hpp"

#include <SDL3/SDL.h>
#include <imgui.h>

#include <cstdint>
#include <optional>
#include <vector>

constexpr ImU32 kColorCanvas = IM_COL32(0x0E, 0x1C, 0x2B, 255);
constexpr ImU32 kColorGrid = IM_COL32(0x2A, 0x4A, 0x6B, 153);
constexpr ImU32 kColorBox = IM_COL32(0xFF, 0x38, 0x38, 255);
constexpr ImU32 kColorGuard = IM_COL32(0x56, 0xB4, 0xE9, 255);
constexpr ImU32 kColorKeep = IM_COL32(0x5E, 0xFF, 0x0D, 255);
constexpr ImU32 kColorPlayer = IM_COL32(0xE6, 0x9F, 0x00, 255);
constexpr ImU32 kColorKept = IM_COL32(0x5E, 0xFF, 0x0D, 140);
constexpr ImU32 kColorDoomed = IM_COL32(0xFF, 0x38, 0x38, 200);

enum class Tool { Select, Keep, Pan };

struct Overlays {
    std::vector<ChunkRect> safehouses;
    std::vector<ChunkRect> keeps;
    std::vector<ChunkRect> players;
};

struct WorldPoint {
    double x = 0;
    double y = 0;
};

struct MapEvents {
    std::optional<ChunkRect> box;
    Tool boxTool = Tool::Select;
    std::optional<WorldPoint> hover;
};

class MapView {
public:
    explicit MapView(SDL_Renderer* renderer) : renderer_(renderer) {}
    ~MapView();
    MapView(const MapView&) = delete;
    MapView& operator=(const MapView&) = delete;

    void setBackground(SDL_Surface* surface);
    void setSave(const Save* save);
    void setSelection(const std::vector<ChunkRect>& rects, const std::vector<std::uint8_t>& doomed, Overlays overlays);
    void setTool(Tool tool) { tool_ = tool; }
    Tool tool() const { return tool_; }
    void zoomBy(double factor);
    void fit() { pendingFit_ = true; }
    MapEvents draw(ImVec2 size);

private:
    struct Drag {
        Tool mode = Tool::Select;
        WorldPoint start;
        ImVec2 startPx;
        std::optional<ChunkRect> rect;
    };

    void applyFit();
    void zoomAt(double factor, double px, double py);
    void paintLayer(const std::vector<std::uint8_t>& doomed);
    void drawTexture(ImDrawList* list, SDL_Texture* texture, ImVec2 a, ImVec2 b, bool nearest) const;
    void drawGrid(ImDrawList* list) const;
    void strokeChunkRect(ImDrawList* list, const ChunkRect& rect, ImU32 color, bool dashed) const;
    ImVec2 toScreen(double worldX, double worldY) const;

    SDL_Renderer* renderer_;
    SDL_Texture* background_ = nullptr;
    SDL_Texture* layer_ = nullptr;
    int layerW_ = 0;
    int layerH_ = 0;
    std::vector<unsigned char> pixels_;
    const Save* save_ = nullptr;
    std::vector<ChunkRect> rects_;
    Overlays overlays_;
    Tool tool_ = Tool::Select;
    double scale_ = 1;
    double originX_ = 0;
    double originY_ = 0;
    ImVec2 canvasMin_;
    ImVec2 canvasMax_;
    bool pendingFit_ = false;
    std::optional<Drag> drag_;
};
