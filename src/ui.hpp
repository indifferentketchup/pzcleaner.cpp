#pragma once

#include "app.hpp"
#include "mapview.hpp"
#include "save.hpp"

#include <SDL3/SDL.h>
#include <nlohmann/json.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

void setUpStyle(float scale);

class Ui {
public:
    Ui(SDL_Window* window, SDL_Renderer* renderer, std::string configDir);
    ~Ui();
    Ui(const Ui&) = delete;
    Ui& operator=(const Ui&) = delete;

    void draw();
    bool animating() const { return opening_ || busy_ || connecting_ || updateState_ == UpdateState::Downloading; }
    bool quitRequested() const { return quit_; }
    bool shutDown();

private:
    enum class Task { None, Scan, Delete, Restore };
    enum class UpdateState { None, Available, Downloading, Ready, Failed };

    struct KeepBox {
        std::string name;
        ChunkRect rect;
    };

    struct SafehouseBox {
        std::string title;
        std::string owner;
        std::size_t members = 0;
        ChunkRect rect;
    };

    struct View {
        Selection selection;
        Overlays overlays;
    };

    struct Confirm {
        std::string title;
        std::string text;
        std::string note;
        std::string action;
        std::function<void()> onOk;
    };

    void background(std::function<void()> work, std::function<void(const std::string&)> done);
    void post(std::function<void()> task);
    void runFinished();
    void resetProgress(Task task);

    void checkForUpdate();
    void startUpdate();
    void openDialog();
    void connect();
    void browseLocal();
    void openLocal();
    void opened(const std::string& name);
    void loadSave(const std::string& name);
    void showSave();
    void refresh();
    void addRect(const ChunkRect& rect, Tool tool);
    void addRectFromForm();
    void boxWholeMap();
    void askConfirm(std::string title, std::string text, std::string note, std::string action,
                    std::function<void()> onOk);
    void runDelete();
    void startDelete(bool useTrash);
    void runRestore();
    void runEmpty();

    std::vector<SafehouseBox> safehouseBoxes() const;
    std::vector<ChunkRect> playerRects() const;
    View currentView() const;
    std::vector<std::string> cellKinds() const;
    std::string planParts(bool trashed) const;
    std::string unreadProtection() const;
    std::string summaryText() const;
    std::string hoverText(const WorldPoint& point) const;
    std::string progressText() const;
    bool locked() const { return opening_ || busy_; }

    void loadSettings();
    void saveSettings();
    void storeKeeps();
    std::vector<KeepBox> loadKeeps(const std::string& name) const;

    void drawHeader();
    void drawUpdate();
    void drawOpen();
    void drawWorkspace();
    void drawBoxLists();
    void drawOptions();
    void drawCommit();
    void drawHosting();
    void drawWhere();
    void drawWelcome();
    void drawMapPane();
    void drawConnect();
    void drawServerTab();
    void drawLocalTab();
    void drawConfirm();

    SDL_Window* window_;
    std::string configDir_;
    App app_;
    MapView map_;
    SDL_Texture* mark_ = nullptr;
    nlohmann::json settings_;

    std::mutex mutex_;
    std::vector<std::function<void()>> finished_;
    std::atomic<int> running_{0};
    std::atomic<std::int64_t> progressDone_{0};
    std::atomic<std::int64_t> progressTotal_{0};
    Task task_ = Task::None;

    std::optional<Save> save_;
    std::vector<ChunkRect> rects_;
    std::vector<KeepBox> keeps_;
    Plan plan_;
    bool dirty_ = false;
    bool canDelete_ = false;
    bool opening_ = false;
    bool busy_ = false;
    bool connecting_ = false;
    bool picking_ = false;

    bool optSafehouses_ = true;
    int padding_ = 2;
    bool optPlayers_ = true;
    int playerPadding_ = 3;
    bool optInvert_ = false;
    std::array<bool, 3> cellKinds_ = {true, true, true};
    bool optVehicles_ = false;
    bool optForever_ = false;

    std::string scanStatus_;
    std::string openError_;
    std::string summary_;
    std::string deleteStatus_;
    std::string deleteError_;
    std::string trashStatus_;
    std::string hover_;
    std::array<std::string, 4> boxFields_;

    bool openConnect_ = false;
    bool closeConnect_ = false;
    std::string host_;
    std::string port_ = "22";
    std::string user_;
    std::string password_;
    std::string path_;
    bool reveal_ = false;
    std::string trust_;
    std::string trustText_;
    std::string connectError_;
    std::string localPath_;
    std::string localError_;

    bool openConfirm_ = false;
    std::optional<Confirm> confirm_;

    UpdateState updateState_ = UpdateState::None;
    std::string updateTag_;
    std::string updateError_;
    std::atomic<std::uint64_t> updateDone_{0};
    std::atomic<std::uint64_t> updateTotal_{0};
    bool quit_ = false;
};
