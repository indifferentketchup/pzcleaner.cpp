#include "mapview.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

constexpr double kZoomStep = 1.2;
constexpr double kMinScale = 0.01;
constexpr double kMaxScale = 16;
constexpr double kGridMinCellPx = 24;
constexpr double kMinDragPx = 4;
constexpr float kDash = 6;
constexpr float kGap = 4;

ImTextureRef textureRef(SDL_Texture* texture) {
    return ImTextureRef(static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(texture)));
}

void putColor(unsigned char* pixel, ImU32 color) {
    pixel[0] = static_cast<unsigned char>((color >> IM_COL32_R_SHIFT) & 0xFF);
    pixel[1] = static_cast<unsigned char>((color >> IM_COL32_G_SHIFT) & 0xFF);
    pixel[2] = static_cast<unsigned char>((color >> IM_COL32_B_SHIFT) & 0xFF);
    pixel[3] = static_cast<unsigned char>((color >> IM_COL32_A_SHIFT) & 0xFF);
}

void dashedLine(ImDrawList* list, ImVec2 a, ImVec2 b, ImU32 color, float low, float high) {
    bool horizontal = a.y == b.y;
    float start = horizontal ? std::min(a.x, b.x) : std::min(a.y, b.y);
    float end = std::min(horizontal ? std::max(a.x, b.x) : std::max(a.y, b.y), high);
    float period = kDash + kGap;
    if (start < low) {
        start += std::floor((low - start) / period) * period;
    }
    for (float s = start; s < end; s += period) {
        float e = std::min(s + kDash, end);
        if (horizontal) {
            list->AddLine(ImVec2(s, a.y), ImVec2(e, a.y), color, 2.0f);
        } else {
            list->AddLine(ImVec2(a.x, s), ImVec2(a.x, e), color, 2.0f);
        }
    }
}

}

MapView::~MapView() {
    if (background_ != nullptr) {
        SDL_DestroyTexture(background_);
    }
    if (layer_ != nullptr) {
        SDL_DestroyTexture(layer_);
    }
}

void MapView::setBackground(SDL_Surface* surface) {
    SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer_, surface);
    while (texture == nullptr && surface->w > 512) {
        SDL_Surface* smaller = SDL_ScaleSurface(surface, surface->w / 2, surface->h / 2, SDL_SCALEMODE_LINEAR);
        SDL_DestroySurface(surface);
        surface = smaller;
        if (surface == nullptr) {
            return;
        }
        texture = SDL_CreateTextureFromSurface(renderer_, surface);
    }
    SDL_DestroySurface(surface);
    if (texture == nullptr) {
        SDL_Log("the world map picture could not be loaded: %s", SDL_GetError());
        return;
    }
    background_ = texture;
}

void MapView::setSave(const Save* save) {
    save_ = save;
    drag_.reset();
    fit();
}

void MapView::setSelection(const std::vector<ChunkRect>& rects, const std::vector<std::uint8_t>& doomed,
                           Overlays overlays) {
    rects_ = rects;
    overlays_ = std::move(overlays);
    paintLayer(doomed);
}

void MapView::paintLayer(const std::vector<std::uint8_t>& doomed) {
    if (save_ == nullptr || !save_->bounds) {
        layerW_ = 0;
        layerH_ = 0;
        return;
    }
    const ChunkRect& bounds = *save_->bounds;
    int width = bounds.x1 - bounds.x0;
    int height = bounds.y1 - bounds.y0;
    if (layer_ == nullptr || width != layerW_ || height != layerH_) {
        if (layer_ != nullptr) {
            SDL_DestroyTexture(layer_);
        }
        layer_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, width, height);
        if (layer_ == nullptr) {
            SDL_Log("the chunk layer could not be made: %s", SDL_GetError());
            layerW_ = 0;
            layerH_ = 0;
            return;
        }
        SDL_SetTextureBlendMode(layer_, SDL_BLENDMODE_BLEND);
        layerW_ = width;
        layerH_ = height;
    }
    pixels_.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4, 0);
    for (std::size_t i = 0; i < save_->chunks.size(); ++i) {
        auto [x, y] = save_->chunks[i];
        std::size_t offset =
            (static_cast<std::size_t>(y - bounds.y0) * static_cast<std::size_t>(width) + (x - bounds.x0)) * 4;
        putColor(&pixels_[offset], doomed[i] ? kColorDoomed : kColorKept);
    }
    SDL_UpdateTexture(layer_, nullptr, pixels_.data(), width * 4);
}

void MapView::applyFit() {
    pendingFit_ = false;
    if (save_ == nullptr || !save_->bounds) {
        return;
    }
    const ChunkRect& bounds = *save_->bounds;
    double width = canvasMax_.x - canvasMin_.x;
    double height = canvasMax_.y - canvasMin_.y;
    double worldW = (bounds.x1 - bounds.x0) * kChunkSquares;
    double worldH = (bounds.y1 - bounds.y0) * kChunkSquares;
    scale_ = std::min(width / worldW, height / worldH) * 0.92;
    originX_ = bounds.x0 * kChunkSquares - (width / scale_ - worldW) / 2;
    originY_ = bounds.y0 * kChunkSquares - (height / scale_ - worldH) / 2;
}

void MapView::zoomAt(double factor, double px, double py) {
    double next = std::clamp(scale_ * factor, kMinScale, kMaxScale);
    originX_ += px / scale_ - px / next;
    originY_ += py / scale_ - py / next;
    scale_ = next;
}

void MapView::zoomBy(double factor) {
    zoomAt(factor, (canvasMax_.x - canvasMin_.x) / 2, (canvasMax_.y - canvasMin_.y) / 2);
}

ImVec2 MapView::toScreen(double worldX, double worldY) const {
    return ImVec2(static_cast<float>(canvasMin_.x + (worldX - originX_) * scale_),
                  static_cast<float>(canvasMin_.y + (worldY - originY_) * scale_));
}

MapEvents MapView::draw(ImVec2 size) {
    MapEvents events;
    ImGuiIO& io = ImGui::GetIO();
    size = ImVec2(std::max(size.x, 1.0f), std::max(size.y, 1.0f));
    canvasMin_ = ImGui::GetCursorScreenPos();
    canvasMax_ = ImVec2(canvasMin_.x + size.x, canvasMin_.y + size.y);
    ImGui::InvisibleButton("map", size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                               ImGuiButtonFlags_MouseButtonMiddle);
    ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
    bool hovered = ImGui::IsItemHovered();
    bool active = ImGui::IsItemActive();
    if (pendingFit_) {
        applyFit();
    }
    if (save_ != nullptr) {
        double px = io.MousePos.x - canvasMin_.x;
        double py = io.MousePos.y - canvasMin_.y;
        if (hovered && io.MouseWheel != 0) {
            zoomAt(std::pow(kZoomStep, io.MouseWheel), px, py);
        }
        WorldPoint point{originX_ + px / scale_, originY_ + py / scale_};
        if (ImGui::IsItemActivated()) {
            Tool mode = tool_;
            if (tool_ == Tool::Pan || !ImGui::IsMouseDown(ImGuiMouseButton_Left) || io.KeyShift) {
                mode = Tool::Pan;
            }
            drag_ = Drag{mode, point, ImVec2(static_cast<float>(px), static_cast<float>(py)), std::nullopt};
        }
        if (drag_ && active) {
            if (drag_->mode == Tool::Pan) {
                originX_ -= io.MouseDelta.x / scale_;
                originY_ -= io.MouseDelta.y / scale_;
            } else if (std::hypot(px - drag_->startPx.x, py - drag_->startPx.y) >= kMinDragPx) {
                drag_->rect = chunkRectBetween(drag_->start.x, drag_->start.y, point.x, point.y);
            }
        }
        if (drag_ && !active) {
            if (drag_->rect) {
                events.box = drag_->rect;
                events.boxTool = drag_->mode;
            }
            drag_.reset();
        }
        if (hovered || active) {
            events.hover = point;
        }
        if (drag_ && drag_->mode == Tool::Pan) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        } else if (hovered && tool_ == Tool::Pan) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }
    }

    ImDrawList* list = ImGui::GetWindowDrawList();
    list->PushClipRect(canvasMin_, canvasMax_, true);
    list->AddRectFilled(canvasMin_, canvasMax_, kColorCanvas);
    if (save_ != nullptr) {
        if (background_ != nullptr) {
            drawTexture(list, background_, toScreen(0, 0), toScreen(kMapW, kMapH), scale_ >= 0.25);
        }
        if (layer_ != nullptr && layerW_ > 0 && save_->bounds) {
            const ChunkRect& bounds = *save_->bounds;
            drawTexture(list, layer_, toScreen(bounds.x0 * kChunkSquares, bounds.y0 * kChunkSquares),
                        toScreen(bounds.x1 * kChunkSquares, bounds.y1 * kChunkSquares), true);
        }
        drawGrid(list);
        for (const ChunkRect& rect : overlays_.players) {
            strokeChunkRect(list, rect, kColorPlayer, false);
        }
        for (const ChunkRect& rect : overlays_.safehouses) {
            strokeChunkRect(list, rect, kColorGuard, false);
        }
        for (const ChunkRect& rect : overlays_.keeps) {
            strokeChunkRect(list, rect, kColorKeep, false);
        }
        for (const ChunkRect& rect : rects_) {
            strokeChunkRect(list, rect, kColorBox, false);
        }
        if (drag_ && drag_->rect) {
            strokeChunkRect(list, *drag_->rect, drag_->mode == Tool::Keep ? kColorKeep : kColorBox, true);
        }
    }
    list->PopClipRect();
    return events;
}

void MapView::drawTexture(ImDrawList* list, SDL_Texture* texture, ImVec2 a, ImVec2 b, bool nearest) const {
    ImVec2 lo(std::max(a.x, canvasMin_.x), std::max(a.y, canvasMin_.y));
    ImVec2 hi(std::min(b.x, canvasMax_.x), std::min(b.y, canvasMax_.y));
    if (lo.x >= hi.x || lo.y >= hi.y) {
        return;
    }
    ImVec2 uv0((lo.x - a.x) / (b.x - a.x), (lo.y - a.y) / (b.y - a.y));
    ImVec2 uv1((hi.x - a.x) / (b.x - a.x), (hi.y - a.y) / (b.y - a.y));
    ImGuiPlatformIO& platform = ImGui::GetPlatformIO();
    list->AddCallback(nearest ? platform.DrawCallback_SetSamplerNearest : platform.DrawCallback_SetSamplerLinear,
                      nullptr);
    list->AddImage(textureRef(texture), lo, hi, uv0, uv1);
    list->AddCallback(platform.DrawCallback_SetSamplerLinear, nullptr);
}

void MapView::drawGrid(ImDrawList* list) const {
    double cell = kCellChunks * kChunkSquares;
    if (cell * scale_ < kGridMinCellPx) {
        return;
    }
    double width = canvasMax_.x - canvasMin_.x;
    double height = canvasMax_.y - canvasMin_.y;
    for (double wx = std::ceil(originX_ / cell) * cell; (wx - originX_) * scale_ < width; wx += cell) {
        float sx = canvasMin_.x + static_cast<float>(std::round((wx - originX_) * scale_)) + 0.5f;
        list->AddLine(ImVec2(sx, canvasMin_.y), ImVec2(sx, canvasMax_.y), kColorGrid, 1.0f);
    }
    for (double wy = std::ceil(originY_ / cell) * cell; (wy - originY_) * scale_ < height; wy += cell) {
        float sy = canvasMin_.y + static_cast<float>(std::round((wy - originY_) * scale_)) + 0.5f;
        list->AddLine(ImVec2(canvasMin_.x, sy), ImVec2(canvasMax_.x, sy), kColorGrid, 1.0f);
    }
}

void MapView::strokeChunkRect(ImDrawList* list, const ChunkRect& rect, ImU32 color, bool dashed) const {
    ImVec2 a = toScreen(rect.x0 * kChunkSquares, rect.y0 * kChunkSquares);
    ImVec2 b = toScreen(rect.x1 * kChunkSquares, rect.y1 * kChunkSquares);
    if (!dashed) {
        list->AddRect(a, b, color, 0.0f, 0, 2.0f);
        return;
    }
    dashedLine(list, a, ImVec2(b.x, a.y), color, canvasMin_.x, canvasMax_.x);
    dashedLine(list, ImVec2(a.x, b.y), b, color, canvasMin_.x, canvasMax_.x);
    dashedLine(list, a, ImVec2(a.x, b.y), color, canvasMin_.y, canvasMax_.y);
    dashedLine(list, ImVec2(b.x, a.y), b, color, canvasMin_.y, canvasMax_.y);
}
