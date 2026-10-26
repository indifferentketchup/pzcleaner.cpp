#include "ui.hpp"

#include "assets.hpp"
#include "text.hpp"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <thread>

namespace {

const char* const kCellKindNames[] = {"zpop", "apop", "chunkdata"};
const char* const kCellKindLabels[] = {"Zombie population (zpop)", "Animal population (apop)",
                                       "Chunk data (chunkdata)"};
const char* const kHostingUrl = "https://indifferentbroccoli.com/project-zomboid-server-hosting";
const char* const kGametoolsUrl = "https://gametools.indifferentketchup.com";

const ImVec4 kBg(0x0A / 255.0f, 0x14 / 255.0f, 0x1E / 255.0f, 1);
const ImVec4 kBgDeep(0x05 / 255.0f, 0x0E / 255.0f, 0x17 / 255.0f, 1);
const ImVec4 kSurface(0x14 / 255.0f, 0x29 / 255.0f, 0x3C / 255.0f, 1);
const ImVec4 kElevated(0x1B / 255.0f, 0x35 / 255.0f, 0x50 / 255.0f, 1);
const ImVec4 kInset(0x0E / 255.0f, 0x1C / 255.0f, 0x2B / 255.0f, 1);
const ImVec4 kBorder(0x2A / 255.0f, 0x4A / 255.0f, 0x6B / 255.0f, 1);
const ImVec4 kBorderStrong(0x3A / 255.0f, 0x5C / 255.0f, 0x80 / 255.0f, 1);
const ImVec4 kText(1, 1, 1, 1);
const ImVec4 kMuted(0xA3 / 255.0f, 0xB8 / 255.0f, 0xCC / 255.0f, 1);
const ImVec4 kFaint(0x8A / 255.0f, 0x9C / 255.0f, 0xAE / 255.0f, 1);
const ImVec4 kBrand(0xFF / 255.0f, 0x38 / 255.0f, 0x38 / 255.0f, 1);
const ImVec4 kBrandHover(0xE6 / 255.0f, 0x2E / 255.0f, 0x2E / 255.0f, 1);
const ImVec4 kInfo(0x56 / 255.0f, 0xB4 / 255.0f, 0xE9 / 255.0f, 1);
const ImVec4 kDanger(0xE9 / 255.0f, 0x75 / 255.0f, 0x15 / 255.0f, 1);

void wakeLoop() {
    SDL_Event event{};
    event.type = SDL_EVENT_USER;
    SDL_PushEvent(&event);
}

std::string trim(const std::string& text) {
    std::size_t start = text.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return "";
    }
    return text.substr(start, text.find_last_not_of(" \t\r\n") - start + 1);
}

std::optional<int> parseInteger(const std::string& text) {
    std::string value = trim(text);
    std::size_t i = !value.empty() && value[0] == '-' ? 1 : 0;
    if (value.size() == i || value.size() - i > 9) {
        return std::nullopt;
    }
    for (std::size_t j = i; j < value.size(); ++j) {
        if (value[j] < '0' || value[j] > '9') {
            return std::nullopt;
        }
    }
    return std::stoi(value);
}

std::string sentence(const std::string& text) {
    if (!text.empty() && (text.back() == '.' || text.back() == '!' || text.back() == '?')) {
        return text;
    }
    return text + ".";
}

std::string describeRect(const ChunkRect& rect) {
    return "x " + std::to_string(rect.x0 * kChunkSquares) + " to " + std::to_string(rect.x1 * kChunkSquares - 1) +
           ", y " + std::to_string(rect.y0 * kChunkSquares) + " to " + std::to_string(rect.y1 * kChunkSquares - 1);
}

template <typename T>
std::string sourceFact(const Listing<T>& source, Noun noun) {
    if (!source.error.empty()) {
        return "could not be read (" + source.error + ")";
    }
    return count(static_cast<std::int64_t>(source.list.size()), noun);
}

void wrapped(const std::string& text) { ImGui::TextWrapped("%s", text.c_str()); }

void coloredWrapped(const ImVec4& color, const std::string& text) {
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    wrapped(text);
    ImGui::PopStyleColor();
}

void hint(const std::string& text) { coloredWrapped(kMuted, text); }

void errorText(const std::string& text) {
    if (!text.empty()) {
        coloredWrapped(kDanger, text);
    }
}

void heading(const char* text, float size) {
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * size);
    ImGui::TextUnformatted(text);
    ImGui::PopFont();
}

bool primaryButton(const char* label) {
    ImGui::PushStyleColor(ImGuiCol_Button, kBrand);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kBrandHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kBrandHover);
    ImGui::PushStyleColor(ImGuiCol_Text, kBg);
    bool clicked = ImGui::Button(label);
    ImGui::PopStyleColor(4);
    return clicked;
}

bool toggleButton(const char* label, bool on) {
    if (on) {
        ImGui::PushStyleColor(ImGuiCol_Button, kBorderStrong);
    }
    bool clicked = ImGui::Button(label);
    if (on) {
        ImGui::PopStyleColor();
    }
    return clicked;
}

void statusPill(const std::string& text, bool pulsing) {
    if (text.empty()) {
        return;
    }
    ImVec4 color = kInfo;
    if (pulsing) {
        color.w = 0.6f + 0.4f * static_cast<float>(std::sin(ImGui::GetTime() * 4.0));
    }
    ImGui::SameLine();
    if (ImGui::GetContentRegionAvail().x < std::min(ImGui::CalcTextSize(text.c_str()).x, ImGui::GetFontSize() * 12)) {
        ImGui::NewLine();
    }
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    wrapped(text);
    ImGui::PopStyleColor();
}

bool wrappedCheckbox(const char* id, const char* label, bool* value) {
    ImGui::PushID(id);
    bool changed = ImGui::Checkbox("##check", value);
    ImGui::SameLine();
    wrapped(label);
    if (ImGui::IsItemClicked()) {
        *value = !*value;
        changed = true;
    }
    ImGui::PopID();
    return changed;
}

void beginPanel(const char* id) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kSurface);
    ImGui::BeginChild(id, ImVec2(0, 0),
                      ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
}

void endPanel() {
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::Spacing();
}

void badge(const char* label) {
    float em = ImGui::GetFontSize();
    ImVec2 pad(em * 0.45f, em * 0.15f);
    ImVec2 textSize = ImGui::CalcTextSize(label);
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImVec2 end(pos.x + textSize.x + pad.x * 2, pos.y + textSize.y + pad.y * 2);
    ImDrawList* list = ImGui::GetWindowDrawList();
    list->AddRectFilled(pos, end, IM_COL32(0x56, 0xB4, 0xE9, 36), em * 0.3f);
    list->AddRect(pos, end, IM_COL32(0x56, 0xB4, 0xE9, 102), em * 0.3f);
    list->AddText(ImVec2(pos.x + pad.x, pos.y + pad.y), ImGui::GetColorU32(kInfo), label);
    ImGui::Dummy(ImVec2(end.x - pos.x, end.y - pos.y));
}

void swatch(ImU32 color, const char* label) {
    float em = ImGui::GetFontSize();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float side = em * 0.75f;
    float top = pos.y + (ImGui::GetTextLineHeight() - side) / 2;
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(pos.x, top), ImVec2(pos.x + side, top + side), color, 2.0f);
    ImGui::Dummy(ImVec2(side, ImGui::GetTextLineHeight()));
    ImGui::SameLine(0, em * 0.35f);
    ImGui::TextUnformatted(label);
    ImGui::SameLine(0, em * 1.1f);
}

void rightAlign(float width) {
    float avail = ImGui::GetContentRegionAvail().x;
    if (avail > width) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - width);
    }
}

bool fieldRow(const char* const* labels, std::string* const* values, const ImGuiInputTextFlags* flags, int count) {
    float spacing = ImGui::GetStyle().ItemSpacing.x;
    float width = (ImGui::GetContentRegionAvail().x - spacing * static_cast<float>(count - 1)) / static_cast<float>(count);
    float left = ImGui::GetCursorPosX();
    for (int i = 0; i < count; ++i) {
        if (i > 0) {
            ImGui::SameLine(left + static_cast<float>(i) * (width + spacing));
        }
        ImGui::TextColored(kMuted, "%s", labels[i]);
    }
    bool entered = false;
    for (int i = 0; i < count; ++i) {
        if (i > 0) {
            ImGui::SameLine(left + static_cast<float>(i) * (width + spacing));
        }
        ImGui::SetNextItemWidth(width);
        ImGui::PushID(i);
        entered |= ImGui::InputText("##field", values[i], flags[i] | ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::PopID();
    }
    return entered;
}

float buttonWidth(const char* label) {
    return ImGui::CalcTextSize(label, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2;
}

nlohmann::json rectJson(const ChunkRect& rect) {
    return {{"x0", rect.x0}, {"y0", rect.y0}, {"x1", rect.x1}, {"y1", rect.y1}};
}

}

void setUpStyle(float scale) {
    ImGuiStyle& style = ImGui::GetStyle();
    ImGui::StyleColorsDark(&style);
    style.WindowPadding = ImVec2(14, 14);
    style.FramePadding = ImVec2(9, 5);
    style.ItemSpacing = ImVec2(8, 7);
    style.ItemInnerSpacing = ImVec2(7, 5);
    style.WindowRounding = 0;
    style.ChildRounding = 10;
    style.PopupRounding = 10;
    style.FrameRounding = 6;
    style.GrabRounding = 6;
    style.TabRounding = 6;
    style.ScrollbarSize = 12;
    style.FrameBorderSize = 1;
    style.ChildBorderSize = 1;
    style.PopupBorderSize = 1;
    style.WindowBorderSize = 0;
    style.SeparatorTextBorderSize = 1;
    style.SeparatorTextPadding = ImVec2(0, 4);
    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text] = kText;
    colors[ImGuiCol_TextDisabled] = kFaint;
    colors[ImGuiCol_WindowBg] = kBg;
    colors[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    colors[ImGuiCol_PopupBg] = kSurface;
    colors[ImGuiCol_Border] = kBorder;
    colors[ImGuiCol_FrameBg] = kInset;
    colors[ImGuiCol_FrameBgHovered] = kElevated;
    colors[ImGuiCol_FrameBgActive] = kElevated;
    colors[ImGuiCol_TitleBg] = kBgDeep;
    colors[ImGuiCol_TitleBgActive] = kSurface;
    colors[ImGuiCol_TitleBgCollapsed] = kBgDeep;
    colors[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    colors[ImGuiCol_ScrollbarGrab] = kBorder;
    colors[ImGuiCol_ScrollbarGrabHovered] = kBorderStrong;
    colors[ImGuiCol_ScrollbarGrabActive] = kBorderStrong;
    colors[ImGuiCol_CheckMark] = ImVec4(0x5E / 255.0f, 1, 0x0D / 255.0f, 1);
    colors[ImGuiCol_Button] = kElevated;
    colors[ImGuiCol_ButtonHovered] = kBorder;
    colors[ImGuiCol_ButtonActive] = kBorderStrong;
    colors[ImGuiCol_Header] = kElevated;
    colors[ImGuiCol_HeaderHovered] = kBorder;
    colors[ImGuiCol_HeaderActive] = kBorderStrong;
    colors[ImGuiCol_Separator] = kBorder;
    colors[ImGuiCol_Tab] = kInset;
    colors[ImGuiCol_TabHovered] = kElevated;
    colors[ImGuiCol_TabSelected] = kElevated;
    colors[ImGuiCol_TabSelectedOverline] = kBrand;
    colors[ImGuiCol_TableBorderLight] = kBorder;
    colors[ImGuiCol_TableBorderStrong] = kBorder;
    colors[ImGuiCol_TextLink] = kInfo;
    colors[ImGuiCol_TextSelectedBg] = ImVec4(kInfo.x, kInfo.y, kInfo.z, 0.35f);
    colors[ImGuiCol_NavCursor] = kInfo;
    colors[ImGuiCol_ModalWindowDimBg] = ImVec4(kBgDeep.x, kBgDeep.y, kBgDeep.z, 0.75f);
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
}

Ui::Ui(SDL_Window* window, SDL_Renderer* renderer, std::string configDir)
    : window_(window),
      configDir_(std::move(configDir)),
      app_(configDir_ + "/known_hosts",
           [this](std::int64_t done, std::int64_t total) {
               progressDone_ = done;
               progressTotal_ = total;
               wakeLoop();
           }),
      map_(renderer) {
    loadSettings();
    if (const Asset* asset = findAsset("broccoli-mark.png")) {
        if (SDL_Surface* surface = SDL_LoadPNG_IO(SDL_IOFromConstMem(asset->data, asset->size), true)) {
            SDL_SetWindowIcon(window_, surface);
            mark_ = SDL_CreateTextureFromSurface(renderer, surface);
            SDL_DestroySurface(surface);
        }
    }
    auto surface = std::make_shared<SDL_Surface*>(nullptr);
    background(
        [surface] {
            if (const Asset* asset = findAsset("worldmap.png")) {
                *surface = SDL_LoadPNG_IO(SDL_IOFromConstMem(asset->data, asset->size), true);
            }
        },
        [this, surface](const std::string&) {
            if (*surface != nullptr) {
                map_.setBackground(*surface);
            }
        });
}

Ui::~Ui() {
    if (mark_ != nullptr) {
        SDL_DestroyTexture(mark_);
    }
}

bool Ui::shutDown() {
    app_.disconnect();
    for (int i = 0; i < 100 && running_ > 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return running_ == 0 && !picking_;
}

void Ui::background(std::function<void()> work, std::function<void(const std::string&)> done) {
    ++running_;
    std::thread([this, work = std::move(work), done = std::move(done)]() mutable {
        std::string error;
        try {
            work();
        } catch (const std::exception& failure) {
            error = failure.what();
            if (error.empty()) {
                error = "something went wrong";
            }
        }
        {
            std::lock_guard lock(mutex_);
            finished_.push_back([done = std::move(done), error] { done(error); });
        }
        wakeLoop();
        --running_;
    }).detach();
}

void Ui::post(std::function<void()> task) {
    {
        std::lock_guard lock(mutex_);
        finished_.push_back(std::move(task));
    }
    wakeLoop();
}

void Ui::runFinished() {
    std::vector<std::function<void()>> tasks;
    {
        std::lock_guard lock(mutex_);
        tasks.swap(finished_);
    }
    for (auto& task : tasks) {
        task();
    }
}

void Ui::resetProgress(Task task) {
    task_ = task;
    progressDone_ = 0;
    progressTotal_ = 0;
}

void Ui::loadSettings() {
    std::ifstream in(std::filesystem::path(configDir_) / "settings.json");
    settings_ = nlohmann::json::parse(in, nullptr, false);
    if (!settings_.is_object()) {
        settings_ = nlohmann::json::object();
    }
    try {
        nlohmann::json connection = settings_.value("connection", nlohmann::json::object());
        host_ = connection.value("host", "");
        port_ = connection.value("port", "22");
        user_ = connection.value("user", "");
        path_ = connection.value("path", "");
        localPath_ = settings_.value("localPath", "");
    } catch (const nlohmann::json::exception&) {
        settings_ = nlohmann::json::object();
    }
}

void Ui::saveSettings() {
    std::error_code ignored;
    std::filesystem::path dir(configDir_);
    std::filesystem::create_directories(dir, ignored);
    std::filesystem::path fresh = dir / "settings.json.new";
    {
        std::ofstream out(fresh, std::ios::binary | std::ios::trunc);
        out << settings_.dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
        if (!out) {
            return;
        }
    }
    std::filesystem::rename(fresh, dir / "settings.json", ignored);
}

void Ui::storeKeeps() {
    nlohmann::json list = nlohmann::json::array();
    for (const KeepBox& keep : keeps_) {
        list.push_back({{"name", keep.name}, {"rect", rectJson(keep.rect)}});
    }
    if (!settings_["keeps"].is_object()) {
        settings_["keeps"] = nlohmann::json::object();
    }
    settings_["keeps"][save_->name] = list;
    saveSettings();
}

std::vector<Ui::KeepBox> Ui::loadKeeps(const std::string& name) const {
    std::vector<KeepBox> keeps;
    auto all = settings_.find("keeps");
    if (all == settings_.end() || !all->is_object() || !all->contains(name) || !(*all)[name].is_array()) {
        return keeps;
    }
    for (const nlohmann::json& keep : (*all)[name]) {
        if (!keep.is_object() || !keep.contains("name") || !keep["name"].is_string() || !keep.contains("rect")) {
            continue;
        }
        const nlohmann::json& rect = keep["rect"];
        bool numbers = rect.is_object();
        for (const char* key : {"x0", "y0", "x1", "y1"}) {
            numbers = numbers && rect.contains(key) && rect[key].is_number_integer();
        }
        if (numbers) {
            keeps.push_back({keep["name"].get<std::string>(),
                             {rect["x0"].get<int>(), rect["y0"].get<int>(), rect["x1"].get<int>(), rect["y1"].get<int>()}});
        }
    }
    return keeps;
}

void Ui::openDialog() {
    openError_.clear();
    connectError_.clear();
    localError_.clear();
    trust_.clear();
    trustText_.clear();
    password_.clear();
    reveal_ = false;
    closeConnect_ = false;
    openConnect_ = true;
}

void Ui::connect() {
    connectError_.clear();
    trustText_.clear();
    ConnectRequest request;
    request.host = trim(host_);
    std::optional<int> port = parseInteger(port_);
    request.port = port && *port >= 1 && *port <= 65535 ? *port : 22;
    request.user = trim(user_);
    request.password = password_;
    request.path = trim(path_);
    request.trust = trust_;
    connecting_ = true;
    auto result = std::make_shared<ConnectResult>();
    background([this, request, result] { *result = app_.connect(request); },
               [this, request, result](const std::string& error) {
                   connecting_ = false;
                   if (!error.empty()) {
                       connectError_ = "Could not connect: " + error;
                       return;
                   }
                   if (!result->fingerprint.empty()) {
                       trust_ = result->fingerprint;
                       trustText_ = "This is the first connection to " + request.host + ". Its " + result->keyType +
                                    " key fingerprint is " + result->fingerprint +
                                    ". Check it against your host's panel or support, then trust it.";
                       return;
                   }
                   settings_["connection"] = {{"host", request.host},
                                              {"port", std::to_string(request.port)},
                                              {"user", request.user},
                                              {"path", request.path}};
                   saveSettings();
                   password_.clear();
                   closeConnect_ = true;
                   opened(result->name);
               });
}

void Ui::browseLocal() {
    localError_.clear();
    picking_ = true;
    SDL_DialogFileCallback picked = [](void* userdata, const char* const* files, int) {
        auto* ui = static_cast<Ui*>(userdata);
        std::string error;
        std::string path;
        if (files == nullptr) {
            error = SDL_GetError();
        } else if (*files != nullptr) {
            path = *files;
        }
        ui->post([ui, error, path] {
            ui->picking_ = false;
            if (!error.empty()) {
                ui->localError_ = "Could not show the folder picker (" + error + "). Type the folder path instead.";
                return;
            }
            if (!path.empty()) {
                ui->localPath_ = path;
                ui->openLocal();
            }
        });
    };
    const char* start = localPath_.empty() ? nullptr : localPath_.c_str();
    SDL_ShowOpenFolderDialog(picked, this, window_, start, false);
}

void Ui::openLocal() {
    localError_.clear();
    connecting_ = true;
    std::string path = trim(localPath_);
    auto name = std::make_shared<std::string>();
    background([this, path, name] { *name = app_.openLocal(path); },
               [this, path, name](const std::string& error) {
                   connecting_ = false;
                   if (!error.empty()) {
                       localError_ = "Could not open the folder: " + error;
                       return;
                   }
                   settings_["localPath"] = path;
                   saveSettings();
                   closeConnect_ = true;
                   opened(*name);
               });
}

void Ui::opened(const std::string& name) {
    deleteStatus_.clear();
    trashStatus_.clear();
    loadSave(name);
}

void Ui::loadSave(const std::string& name) {
    opening_ = true;
    resetProgress(Task::Scan);
    auto loaded = std::make_shared<Save>();
    background([this, loaded, name] { *loaded = openSave(app_, name); },
               [this, loaded](const std::string& error) {
                   opening_ = false;
                   task_ = Task::None;
                   if (!error.empty()) {
                       scanStatus_.clear();
                       openError_ = error;
                       save_.reset();
                       map_.setSave(nullptr);
                       return;
                   }
                   save_ = std::move(*loaded);
                   rects_.clear();
                   keeps_ = loadKeeps(save_->name);
                   scanStatus_ = save_->name + ": " + count(static_cast<std::int64_t>(save_->chunks.size()), Noun::Chunk);
                   showSave();
               });
}

void Ui::showSave() {
    if (!save_) {
        return;
    }
    map_.setSave(&*save_);
    refresh();
}

std::vector<Ui::SafehouseBox> Ui::safehouseBoxes() const {
    std::vector<SafehouseBox> boxes;
    if (!optSafehouses_) {
        return boxes;
    }
    int padding = std::clamp(padding_, 0, 50);
    for (const Safehouse& house : save_->safehouses.list) {
        boxes.push_back({house.title, house.owner, house.players.size(),
                         squaresToChunkRect(house.x, house.y, house.w, house.h, padding)});
    }
    return boxes;
}

std::vector<ChunkRect> Ui::playerRects() const {
    std::vector<ChunkRect> rects;
    if (!optPlayers_) {
        return rects;
    }
    int padding = std::clamp(playerPadding_, 0, 50);
    for (const Player& player : save_->players.list) {
        rects.push_back(squaresToChunkRect(player.x, player.y, 1, 1, padding));
    }
    return rects;
}

Ui::View Ui::currentView() const {
    View view;
    for (const SafehouseBox& house : safehouseBoxes()) {
        view.overlays.safehouses.push_back(house.rect);
    }
    view.overlays.players = playerRects();
    for (const KeepBox& keep : keeps_) {
        view.overlays.keeps.push_back(keep.rect);
    }
    view.selection.rects = rects_;
    view.selection.inverted = optInvert_;
    for (const auto* rects : {&view.overlays.safehouses, &view.overlays.players, &view.overlays.keeps}) {
        view.selection.protectedRects.insert(view.selection.protectedRects.end(), rects->begin(), rects->end());
    }
    return view;
}

std::vector<std::string> Ui::cellKinds() const {
    std::vector<std::string> kinds;
    for (std::size_t i = 0; i < cellKinds_.size(); ++i) {
        if (cellKinds_[i]) {
            kinds.emplace_back(kCellKindNames[i]);
        }
    }
    return kinds;
}

std::string Ui::planParts(bool trashed) const {
    std::size_t cellFiles = plan_.clearedCells.size() * plan_.cellKinds.size();
    std::vector<std::string> parts = {count(static_cast<std::int64_t>(plan_.chunkCount), Noun::Chunk) + " out of " +
                                      formatNumber(static_cast<std::int64_t>(save_->chunks.size()))};
    if (cellFiles > 0) {
        parts.push_back("up to " + count(static_cast<std::int64_t>(cellFiles), Noun::CellFile) + " for the " +
                        count(static_cast<std::int64_t>(plan_.clearedCells.size()), Noun::Cell) + " left empty");
    }
    if (plan_.vehicleCount > 0) {
        parts.push_back(count(static_cast<std::int64_t>(plan_.vehicleCount), Noun::Vehicle));
    }
    if (trashed) {
        return "This moves to the trash " + joinList(parts) + ".";
    }
    return "This deletes " + joinList(parts) + ".";
}

std::string Ui::unreadProtection() const {
    if (optSafehouses_ && !save_->safehouses.error.empty()) {
        return "map_meta.bin could not be read (" + save_->safehouses.error +
               "), so the safehouses can't be protected. Untick \"Claimed safehouses\" to delete without that "
               "protection.";
    }
    if (optPlayers_ && !save_->players.error.empty()) {
        return "players.db could not be read (" + save_->players.error +
               "), so the players can't be protected. Untick \"Where living players last stood\" to delete without "
               "that protection.";
    }
    return "";
}

std::string Ui::summaryText() const {
    if (rects_.empty()) {
        return "Draw a delete box on the map to choose what to reset.";
    }
    if (plan_.chunkCount == 0) {
        return "Nothing to delete: no saved chunks fall inside that selection.";
    }
    return planParts(!optForever_);
}

void Ui::refresh() {
    dirty_ = false;
    if (!save_) {
        return;
    }
    if (save_->chunks.empty()) {
        plan_ = Plan();
        summary_ = "This save has no chunk files left.";
        canDelete_ = false;
        return;
    }
    View view = currentView();
    plan_ = buildPlan(*save_, view.selection, cellKinds(), optVehicles_);
    map_.setSelection(rects_, plan_.doomed, std::move(view.overlays));
    std::string problem = unreadProtection();
    if (!problem.empty()) {
        summary_ = problem;
        canDelete_ = false;
        return;
    }
    summary_ = summaryText();
    canDelete_ = plan_.chunkCount > 0;
}

void Ui::addRect(const ChunkRect& rect, Tool tool) {
    if (tool == Tool::Keep) {
        keeps_.push_back({"Keep box " + std::to_string(keeps_.size() + 1), rect});
        storeKeeps();
    } else {
        rects_.push_back(rect);
    }
    refresh();
}

void Ui::addRectFromForm() {
    std::array<int, 4> values{};
    for (std::size_t i = 0; i < boxFields_.size(); ++i) {
        std::optional<int> value = parseInteger(boxFields_[i]);
        if (!value) {
            return;
        }
        values[i] = *value;
    }
    addRect(chunkRectBetween(values[0], values[1], values[2], values[3]), Tool::Select);
    for (std::string& field : boxFields_) {
        field.clear();
    }
}

void Ui::boxWholeMap() {
    const ChunkRect& bounds = *save_->bounds;
    addRect({std::min(0, bounds.x0), std::min(0, bounds.y0), std::max(kMapW / kChunkSquares, bounds.x1),
             std::max(kMapH / kChunkSquares, bounds.y1)},
            Tool::Select);
}

std::string Ui::hoverText(const WorldPoint& point) const {
    int x = static_cast<int>(std::floor(point.x));
    int y = static_cast<int>(std::floor(point.y));
    int chunkX = floorDiv(x, kChunkSquares);
    int chunkY = floorDiv(y, kChunkSquares);
    std::string text = "x " + std::to_string(x) + ", y " + std::to_string(y) + " \xC2\xB7 chunk " +
                       std::to_string(chunkX) + "," + std::to_string(chunkY) + " \xC2\xB7 cell " +
                       std::to_string(floorDiv(chunkX, kCellChunks)) + "," +
                       std::to_string(floorDiv(chunkY, kCellChunks));
    for (const SafehouseBox& house : safehouseBoxes()) {
        if (rectContains(house.rect, chunkX, chunkY)) {
            text += " \xC2\xB7 " + house.title + ", owner " + house.owner + ", " +
                    count(static_cast<std::int64_t>(house.members), Noun::Member);
            break;
        }
    }
    return text;
}

std::string Ui::progressText() const {
    std::int64_t done = progressDone_;
    std::int64_t total = progressTotal_;
    switch (task_) {
    case Task::Scan:
        if (total > 0) {
            return "reading map folders, " + formatNumber(done) + " of " + formatNumber(total);
        }
        return "reading the folder";
    case Task::Delete:
        if (total > 0) {
            return "working, " + formatNumber(done) + " of " + formatNumber(total);
        }
        return "working";
    case Task::Restore:
        return "putting back, " + formatNumber(done);
    default:
        return "";
    }
}

void Ui::askConfirm(std::string title, std::string text, std::string note, std::string action,
                    std::function<void()> onOk) {
    confirm_ = Confirm{std::move(title), std::move(text), std::move(note), std::move(action), std::move(onOk)};
    openConfirm_ = true;
}

void Ui::runDelete() {
    if (!save_ || plan_.chunkCount == 0) {
        return;
    }
    bool useTrash = !optForever_;
    std::string where = " Folder: \"" + save_->name + "\".";
    if (useTrash) {
        askConfirm("Move these files to the trash?", planParts(true) + where,
                   "You can put them back from the trash until you empty it. Make sure the server is stopped.",
                   "Move to trash", [this] { startDelete(true); });
        return;
    }
    askConfirm("Delete these files?", planParts(false) + where,
               "This can't be undone from here. Make sure the server is stopped and you have a backup.", "Delete",
               [this] { startDelete(false); });
}

void Ui::startDelete(bool useTrash) {
    deleteError_.clear();
    trashStatus_.clear();
    busy_ = true;
    resetProgress(Task::Delete);
    ExecuteRequest request;
    for (std::size_t i = 0; i < save_->chunks.size(); ++i) {
        if (plan_.doomed[i]) {
            request.chunks.push_back(save_->chunks[i]);
        }
    }
    request.cells = plan_.clearedCells;
    request.cellKinds = plan_.cellKinds;
    request.vehicles = plan_.vehicleCount > 0;
    request.trash = useTrash;
    auto result = std::make_shared<ExecuteResult>();
    auto trash = std::make_shared<std::vector<std::string>>();
    std::size_t chunkCount = request.chunks.size();
    background(
        [this, request, result, trash] {
            *result = app_.execute(request);
            if (request.trash) {
                *trash = app_.listTrash();
            }
        },
        [this, result, trash, doomed = plan_.doomed, chunkCount, useTrash](const std::string& error) {
            busy_ = false;
            task_ = Task::None;
            if (!error.empty()) {
                deleteStatus_.clear();
                deleteError_ = "Stopped: " + sentence(error) +
                               " Anything already removed stays removed. Open the folder again to see what is left.";
                showSave();
                return;
            }
            if (save_ && save_->chunks.size() == doomed.size()) {
                save_->dropChunks(doomed);
                save_->dropVehicles(result->vehicleIds);
                if (useTrash) {
                    save_->trash = *trash;
                }
            }
            std::vector<std::string> parts = {count(static_cast<std::int64_t>(chunkCount), Noun::Chunk),
                                              count(result->cellFiles, Noun::CellFile)};
            if (!result->vehicleIds.empty()) {
                parts.push_back(count(static_cast<std::int64_t>(result->vehicleIds.size()), Noun::Vehicle));
            }
            deleteStatus_ = (useTrash ? "moved to the trash: " : "deleted: ") + joinList(parts);
            rects_.clear();
            showSave();
        });
}

void Ui::runRestore() {
    std::string stamp = save_->trash.back();
    askConfirm("Put the latest batch back?",
               "Everything in the trash batch from " + formatStamp(stamp) + " moves back into \"" + save_->name + "\".",
               "Files with the same name are replaced, including vehicles.db if that batch holds one. Make sure the "
               "server is stopped.",
               "Put back", [this, stamp] {
                   busy_ = true;
                   resetProgress(Task::Restore);
                   deleteError_.clear();
                   deleteStatus_.clear();
                   auto moved = std::make_shared<std::int64_t>(0);
                   background([this, stamp, moved] { *moved = app_.restore(stamp); },
                              [this, moved](const std::string& error) {
                                  busy_ = false;
                                  task_ = Task::None;
                                  if (error.empty()) {
                                      trashStatus_ = "put back " + count(*moved, Noun::File);
                                  } else {
                                      trashStatus_.clear();
                                      deleteError_ = "Restore stopped: " + error;
                                  }
                                  loadSave(save_->name);
                              });
               });
}

void Ui::runEmpty() {
    askConfirm("Empty the trash for good?",
               count(static_cast<std::int64_t>(save_->trash.size()), Noun::Batch) + " in \"" + save_->name +
                   "\" will be deleted.",
               "This can't be undone.", "Empty trash", [this] {
                   busy_ = true;
                   deleteError_.clear();
                   background([this] { app_.emptyTrash(); },
                              [this](const std::string& error) {
                                  busy_ = false;
                                  if (error.empty()) {
                                      save_->trash.clear();
                                      trashStatus_.clear();
                                  } else {
                                      deleteError_ = "Could not empty the trash: " + error;
                                  }
                                  refresh();
                              });
               });
}

void Ui::draw() {
    runFinished();
    if (dirty_) {
        refresh();
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin("pzcleaner", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse);
    drawHeader();
    float em = ImGui::GetFontSize();
    float side = std::clamp(ImGui::GetContentRegionAvail().x * 0.34f, em * 26, em * 34);
    ImGui::BeginChild("side", ImVec2(side, 0));
    drawOpen();
    bool hasChunks = save_ && !save_->chunks.empty();
    if (hasChunks) {
        drawWorkspace();
    }
    if (save_) {
        drawCommit();
        if (ImGui::CollapsingHeader("If you can't find the save folder")) {
            drawWhere();
        }
    }
    drawHosting();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("main", ImVec2(0, 0), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    if (hasChunks) {
        drawMapPane();
    } else {
        drawWelcome();
    }
    ImGui::EndChild();
    drawConnect();
    drawConfirm();
    ImGui::End();
}

void Ui::drawHeader() {
    float em = ImGui::GetFontSize();
    if (mark_ != nullptr) {
        ImGui::Image(ImTextureRef(static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(mark_))),
                     ImVec2(em * 2.4f, em * 2.4f));
        ImGui::SameLine();
    }
    ImGui::BeginGroup();
    heading("pzcleaner:", 1.5f);
    ImGui::SameLine();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + em * 0.35f);
    badge("B42");
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::TextUnformatted("indifferent broccoli chunk cleaner tool for project zomboid");
    ImGui::PopStyleColor();
    ImGui::EndGroup();
    ImGui::Spacing();
}

void Ui::drawOpen() {
    beginPanel("open");
    heading("1. Open your save folder", 1.2f);
    wrapped("Stop the server first, and keep a copy of the save. Deleted chunk files do not go to the recycle bin.");
    ImGui::BeginDisabled(locked());
    if (primaryButton("Open a save")) {
        openDialog();
    }
    ImGui::EndDisabled();
    if (opening_) {
        statusPill(progressText(), true);
    } else {
        statusPill(scanStatus_, false);
    }
    hint("Log in with your server's SFTP details, or pick a save folder on this computer. It's the folder that holds "
         "map_meta.bin.");
    errorText(openError_);
    endPanel();
}

void Ui::drawWorkspace() {
    beginPanel("pick");
    heading("2. Box the areas to reset", 1.2f);
    if (ImGui::BeginTable("facts", 2, ImGuiTableFlags_SizingFixedFit)) {
        const std::pair<const char*, std::string> rows[] = {
            {"format", "Build 42"},
            {"saved chunks", formatNumber(static_cast<std::int64_t>(save_->chunks.size()))},
            {"explored area", describeRect(*save_->bounds)},
            {"safehouses", sourceFact(save_->safehouses, Noun::Safehouse)},
            {"players", sourceFact(save_->players, Noun::Player)},
            {"vehicles", sourceFact(save_->vehicles, Noun::Vehicle)},
        };
        ImGui::TableSetupColumn("term", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("detail", ImGuiTableColumnFlags_WidthStretch);
        for (const auto& [term, detail] : rows) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextColored(kMuted, "%s", term);
            ImGui::TableNextColumn();
            wrapped(detail);
        }
        ImGui::EndTable();
    }

    ImGui::SeparatorText("add a delete box by in-game coordinates");
    const char* const labels[] = {"from x", "from y", "to x", "to y"};
    std::string* const values[] = {&boxFields_[0], &boxFields_[1], &boxFields_[2], &boxFields_[3]};
    const ImGuiInputTextFlags decimal[] = {ImGuiInputTextFlags_CharsDecimal, ImGuiInputTextFlags_CharsDecimal,
                                           ImGuiInputTextFlags_CharsDecimal, ImGuiInputTextFlags_CharsDecimal};
    ImGui::PushID("boxform");
    bool submit = fieldRow(labels, values, decimal, 4);
    ImGui::PopID();
    bool valid = std::all_of(boxFields_.begin(), boxFields_.end(),
                             [](const std::string& field) { return parseInteger(field).has_value(); });
    ImGui::BeginDisabled(!valid);
    if (primaryButton("Add box") || (submit && valid)) {
        addRectFromForm();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Box the whole map")) {
        boxWholeMap();
    }
    drawBoxLists();
    drawOptions();
    endPanel();
}

void Ui::drawBoxLists() {
    ImGui::PushID("delete");
    for (std::size_t i = 0; i < rects_.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        ImGui::AlignTextToFramePadding();
        ImGui::Text("delete: %s", describeRect(rects_[i]).c_str());
        ImGui::SameLine();
        bool remove = ImGui::SmallButton("Remove");
        ImGui::PopID();
        if (remove) {
            rects_.erase(rects_.begin() + static_cast<std::ptrdiff_t>(i));
            refresh();
            break;
        }
    }
    ImGui::PopID();
    ImGui::PushID("keep");
    for (std::size_t i = 0; i < keeps_.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
        ImGui::InputText("##name", &keeps_[i].name);
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            storeKeeps();
        }
        ImGui::SameLine();
        ImGui::Text("keep: %s", describeRect(keeps_[i].rect).c_str());
        ImGui::SameLine();
        bool remove = ImGui::SmallButton("Remove");
        ImGui::PopID();
        if (remove) {
            keeps_.erase(keeps_.begin() + static_cast<std::ptrdiff_t>(i));
            storeKeeps();
            refresh();
            break;
        }
    }
    ImGui::PopID();
}

void Ui::drawOptions() {
    bool changed = false;
    float em = ImGui::GetFontSize();
    ImGui::SeparatorText("never delete");
    changed |= wrappedCheckbox("safehouses", "Claimed safehouses", &optSafehouses_);
    ImGui::Indent(em * 1.8f);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("chunks of padding around each safehouse");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(em * 6);
    changed |= ImGui::InputInt("##padding", &padding_, 1, 5);
    ImGui::Unindent(em * 1.8f);
    changed |= wrappedCheckbox("players", "Where living players last stood", &optPlayers_);
    ImGui::Indent(em * 1.8f);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("chunks of padding around each player");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(em * 6);
    changed |= ImGui::InputInt("##playerpadding", &playerPadding_, 1, 5);
    ImGui::Unindent(em * 1.8f);
    padding_ = std::clamp(padding_, 0, 50);
    playerPadding_ = std::clamp(playerPadding_, 0, 50);
    hint("Keep boxes are always protected, and are remembered on this computer for a save folder with the same name.");

    ImGui::SeparatorText("selection");
    changed |= wrappedCheckbox("invert", "Keep what's inside the delete boxes and delete everything else", &optInvert_);

    ImGui::SeparatorText("in cells left with no chunk files, also delete");
    for (std::size_t i = 0; i < cellKinds_.size(); ++i) {
        changed |= wrappedCheckbox(kCellKindNames[i], kCellKindLabels[i], &cellKinds_[i]);
    }

    ImGui::SeparatorText("vehicles");
    changed |= wrappedCheckbox("vehicles", "Remove vehicles parked in the deleted chunks", &optVehicles_);
    hint("The game only spawns fresh vehicles in a chunk that has none on record, so leave this off and the old ones "
         "stay put, including players' cars. Ticking it edits vehicles.db and keeps a copy of the original.");

    ImGui::SeparatorText("undo");
    changed |= wrappedCheckbox("forever", "Delete for good (faster)", &optForever_);
    hint("Unticked, files move to a trash folder, pzcleaner-backup in the Zomboid folder beside Saves, so they can be "
         "put back. That is slower on big selections, and it takes up the same disk space until you empty it.");
    if (changed) {
        dirty_ = true;
    }
}

void Ui::drawCommit() {
    beginPanel("commit");
    heading("3. Delete", 1.2f);
    wrapped(summary_);
    ImGui::BeginDisabled(locked() || !canDelete_);
    if (primaryButton(optForever_ ? "Delete files###delete" : "Move files to trash###delete")) {
        runDelete();
    }
    ImGui::EndDisabled();
    if (task_ == Task::Delete) {
        statusPill(progressText(), true);
    } else {
        statusPill(deleteStatus_, false);
    }
    errorText(deleteError_);
    if (!save_->trash.empty()) {
        ImGui::SeparatorText("trash");
        wrapped("The trash folder holds " + count(static_cast<std::int64_t>(save_->trash.size()), Noun::Batch) +
                ". The latest is from " + formatStamp(save_->trash.back()) + ".");
        ImGui::BeginDisabled(locked());
        if (ImGui::Button("Put the latest batch back")) {
            runRestore();
        }
        ImGui::SameLine();
        if (primaryButton("Empty the trash for good")) {
            runEmpty();
        }
        ImGui::EndDisabled();
        if (task_ == Task::Restore) {
            statusPill(progressText(), true);
        } else {
            statusPill(trashStatus_, false);
        }
    }
    endPanel();
}

void Ui::drawHosting() {
    beginPanel("hosting");
    heading("run your own pz server", 1.1f);
    hint("If you don't already have a server for this map, Indifferent Broccoli rents Project Zomboid servers. No port "
         "forwarding, and the save files are a click away in the file manager.");
    if (primaryButton("launch 2-day free trial server")) {
        SDL_OpenURL(kHostingUrl);
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(kMuted, "see our other game tools");
    ImGui::SameLine();
    if (ImGui::Button("gametools")) {
        SDL_OpenURL(kGametoolsUrl);
    }
    endPanel();
}

void Ui::drawWhere() {
    wrapped("The folder is named after the save, or after the server for multiplayer. It's the one with map_meta.bin "
            "and a map folder in it.");
    const std::pair<const char*, const char*> places[] = {
        {"Rented or dedicated server",
         "Open a save and log in with the SFTP details from your host's panel. The save is "
         "Zomboid/Saves/Multiplayer/<server name>, and on some hosts the Zomboid folder has another name, such as "
         "server-data."},
        {"Windows", "Paste %USERPROFILE%\\Zomboid\\Saves into the folder picker's address bar, then open the game mode "
                    "and the save."},
        {"Linux", "~/Zomboid/Saves, same folders as Windows."},
    };
    for (const auto& [place, text] : places) {
        ImGui::TextColored(kMuted, "%s", place);
        ImGui::Indent();
        wrapped(text);
        ImGui::Unindent();
    }
}

void Ui::drawWelcome() {
    ImGui::Indent(ImGui::GetFontSize());
    ImGui::Dummy(ImVec2(0, ImGui::GetFontSize()));
    ImGui::PushTextWrapPos(std::min(ImGui::GetContentRegionAvail().x, ImGui::GetFontSize() * 44));
    heading("Reset part of a Project Zomboid map", 1.6f);
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    wrapped("For Build 42 saves. Every chunk a player has walked through is saved as its own file. "
                             "Delete the file and the game builds that chunk fresh the next time someone gets close: "
                             "loot back, zombies back, player damage gone. This app shows which chunks your save has, "
                             "lets you box the ones to reset, and deletes those files for you.");
    ImGui::PopStyleColor();
    if (save_) {
        ImGui::Spacing();
        ImGui::TextUnformatted("This save has no chunk files left.");
    }
    ImGui::Dummy(ImVec2(0, ImGui::GetFontSize()));
    heading("If you can't find the save folder", 1.2f);
    drawWhere();
    ImGui::PopTextWrapPos();
    ImGui::Unindent(ImGui::GetFontSize());
}

void Ui::drawMapPane() {
    Tool tool = map_.tool();
    if (toggleButton("Draw delete box", tool == Tool::Select)) {
        map_.setTool(Tool::Select);
    }
    ImGui::SameLine();
    if (toggleButton("Draw keep box", tool == Tool::Keep)) {
        map_.setTool(Tool::Keep);
    }
    ImGui::SameLine();
    if (toggleButton("Pan", tool == Tool::Pan)) {
        map_.setTool(Tool::Pan);
    }
    ImGui::SameLine();
    float zoomWidth = buttonWidth("-") + buttonWidth("+") + buttonWidth("Fit") + ImGui::GetStyle().ItemSpacing.x * 2;
    rightAlign(zoomWidth);
    if (ImGui::Button("-")) {
        map_.zoomBy(1 / 1.5);
    }
    ImGui::SameLine();
    if (ImGui::Button("+")) {
        map_.zoomBy(1.5);
    }
    ImGui::SameLine();
    if (ImGui::Button("Fit")) {
        map_.fit();
    }

    const char* hintText = "Drag to draw a box. Scroll to zoom. Hold Shift and drag, or right-drag, to pan. \"Box the "
                           "whole map\" covers the whole world, modded areas included. Only saved chunks are deleted, "
                           "and they turn red.";
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float hintHeight = ImGui::CalcTextSize(hintText, nullptr, false, avail.x).y;
    float footer = ImGui::GetTextLineHeightWithSpacing() * 2 + hintHeight + ImGui::GetStyle().ItemSpacing.y * 2;
    MapEvents events = map_.draw(ImVec2(avail.x, avail.y - footer));
    if (events.box) {
        addRect(*events.box, events.boxTool);
    }
    if (events.hover) {
        hover_ = hoverText(*events.hover);
    }
    swatch(kColorKept, "saved chunk");
    swatch(kColorDoomed, "will be deleted");
    swatch(kColorGuard, "safehouse");
    swatch(kColorKeep, "keep box");
    swatch(kColorPlayer, "player");
    ImGui::NewLine();
    ImGui::TextColored(kMuted, "%s", hover_.c_str());
    hint(hintText);
}

void Ui::drawConnect() {
    if (openConnect_) {
        ImGui::OpenPopup("Open a save###connect");
        openConnect_ = false;
    }
    float em = ImGui::GetFontSize();
    ImGui::SetNextWindowSize(ImVec2(em * 36, 0));
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("Open a save###connect", nullptr, ImGuiWindowFlags_NoSavedSettings)) {
        return;
    }
    if (closeConnect_) {
        closeConnect_ = false;
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    if (ImGui::BeginTabBar("source")) {
        if (ImGui::BeginTabItem("Game server (SFTP)")) {
            drawServerTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("This computer")) {
            drawLocalTab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    if (!connecting_ && !picking_ && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        password_.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void Ui::drawServerTab() {
    wrapped("For a rented server, use the SFTP login from your host's panel. Stop the game server first. The save "
            "folder is the one holding map_meta.bin and a map folder.");
    ImGui::Spacing();
    ImGui::BeginDisabled(connecting_);
    if (ImGui::IsWindowAppearing()) {
        ImGui::SetKeyboardFocusHere();
    }
    const char* const hostLabels[] = {"Host", "Port"};
    std::string* const hostValues[] = {&host_, &port_};
    const ImGuiInputTextFlags hostFlags[] = {ImGuiInputTextFlags_None, ImGuiInputTextFlags_CharsDecimal};
    ImGui::PushID("host");
    fieldRow(hostLabels, hostValues, hostFlags, 2);
    ImGui::PopID();
    const char* const userLabels[] = {"User", "Password"};
    std::string* const userValues[] = {&user_, &password_};
    const ImGuiInputTextFlags userFlags[] = {ImGuiInputTextFlags_None,
                                             reveal_ ? ImGuiInputTextFlags_None : ImGuiInputTextFlags_Password};
    ImGui::PushID("user");
    fieldRow(userLabels, userValues, userFlags, 2);
    ImGui::PopID();
    ImGui::Checkbox("Show password", &reveal_);
    ImGui::TextColored(kMuted, "Save folder on the server");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##path", "/home/pz/Zomboid/Saves/Multiplayer/servertest", &path_);
    ImGui::EndDisabled();
    if (!trustText_.empty()) {
        wrapped(trustText_);
    }
    errorText(connectError_);
    ImGui::Spacing();
    const char* action = connecting_ ? "Connecting\xE2\x80\xA6###go" : trust_.empty() ? "Connect###go" : "Trust and connect###go";
    float width = buttonWidth("Cancel") + buttonWidth(action) + ImGui::GetStyle().ItemSpacing.x;
    rightAlign(width);
    ImGui::BeginDisabled(connecting_);
    if (ImGui::Button("Cancel")) {
        password_.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    bool enter = ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter);
    if (primaryButton(action) || (enter && !connecting_)) {
        connect();
    }
    ImGui::EndDisabled();
}

void Ui::drawLocalTab() {
    wrapped("Pick the save folder on this computer, the one holding map_meta.bin and a map folder.");
    ImGui::Spacing();
    ImGui::BeginDisabled(connecting_ || picking_);
    ImGui::TextColored(kMuted, "Save folder");
    const char* browse = "Browse\xE2\x80\xA6";
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - buttonWidth(browse) - ImGui::GetStyle().ItemSpacing.x);
    ImGui::InputText("##local", &localPath_);
    ImGui::SameLine();
    if (ImGui::Button(browse)) {
        browseLocal();
    }
    ImGui::EndDisabled();
    errorText(localError_);
    ImGui::Spacing();
    float width = buttonWidth("Cancel") + buttonWidth("Open") + ImGui::GetStyle().ItemSpacing.x;
    rightAlign(width);
    ImGui::BeginDisabled(connecting_ || picking_);
    if (ImGui::Button("Cancel")) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    bool enter = ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter);
    if (primaryButton("Open") || (enter && !connecting_ && !picking_)) {
        openLocal();
    }
    ImGui::EndDisabled();
}

void Ui::drawConfirm() {
    if (openConfirm_) {
        ImGui::OpenPopup("###confirm");
        openConfirm_ = false;
    }
    if (!confirm_) {
        return;
    }
    float em = ImGui::GetFontSize();
    ImGui::SetNextWindowSize(ImVec2(em * 30, 0));
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    std::string title = confirm_->title + "###confirm";
    if (!ImGui::BeginPopupModal(title.c_str(), nullptr, ImGuiWindowFlags_NoSavedSettings)) {
        return;
    }
    wrapped(confirm_->text);
    ImGui::Spacing();
    hint(confirm_->note);
    ImGui::Spacing();
    std::string action = confirm_->action + "###ok";
    float width = buttonWidth("Cancel") + buttonWidth(action.c_str()) + ImGui::GetStyle().ItemSpacing.x;
    rightAlign(width);
    bool close = ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape);
    ImGui::SameLine();
    std::function<void()> onOk;
    if (primaryButton(action.c_str())) {
        onOk = confirm_->onOk;
        close = true;
    }
    if (close) {
        confirm_.reset();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
    if (onOk) {
        onOk();
    }
}
