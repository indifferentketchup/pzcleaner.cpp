#include "assets.hpp"
#include "ui.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_internal.h>
#include <imgui_impl_sdlrenderer3.h>
#include <libssh2.h>

#include <algorithm>
#include <cstdlib>
#include <string>

#ifdef _WIN32
#include <winsock2.h>
#endif

namespace {

constexpr float kFontSize = 16.0f;

std::string configDir() {
#ifdef _WIN32
    const char* appData = std::getenv("APPDATA");
    return std::string(appData ? appData : ".") + "/pzcleaner";
#elif defined(__APPLE__)
    const char* home = std::getenv("HOME");
    return std::string(home ? home : ".") + "/Library/Application Support/pzcleaner";
#else
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg != nullptr && *xdg != '\0') {
        return std::string(xdg) + "/pzcleaner";
    }
    const char* home = std::getenv("HOME");
    return std::string(home ? home : ".") + "/.config/pzcleaner";
#endif
}

int fail(const char* what) {
    std::string message = std::string(what) + ": " + SDL_GetError();
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "pzcleaner", message.c_str(), nullptr);
    return 1;
}

int run() {
    SDL_SetAppMetadata("pzcleaner", nullptr, "com.indifferentketchup.pzcleaner");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        return fail("the window system could not start");
    }
    float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    if (scale <= 0) {
        scale = 1;
    }
    SDL_Window* window =
        SDL_CreateWindow("pzcleaner", static_cast<int>(1400 * scale), static_cast<int>(900 * scale),
                         SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (window == nullptr) {
        return fail("the window could not be made");
    }
    SDL_SetWindowMinimumSize(window, static_cast<int>(960 * scale), static_cast<int>(640 * scale));
    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
    if (renderer == nullptr) {
        return fail("the window could not be drawn");
    }
    SDL_SetRenderVSync(renderer, 1);
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    SDL_ShowWindow(window);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    setUpStyle(scale);
    if (const Asset* font = findAsset("Roboto-Medium.ttf")) {
        ImFontConfig config;
        config.FontDataOwnedByAtlas = false;
        io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(font->data), static_cast<int>(font->size), kFontSize,
                                       &config);
    }
    ImGui::GetStyle().FontSizeBase = kFontSize;
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    {
        Ui ui(window, renderer, configDir());
        bool done = false;
        int frames = 3;
        auto handle = [&](const SDL_Event& event) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT || (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                                                 event.window.windowID == SDL_GetWindowID(window))) {
                done = true;
            }
            frames = 3;
        };
        while (!done) {
            SDL_Event event;
            bool minimized = (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) != 0;
            if (frames == 0 || minimized) {
                int timeout = ui.animating() ? 100 : io.WantTextInput ? 250 : 1000;
                if (SDL_WaitEventTimeout(&event, timeout)) {
                    handle(event);
                }
            }
            while (SDL_PollEvent(&event)) {
                handle(event);
            }
            if (done || minimized) {
                continue;
            }
            ImGui_ImplSDLRenderer3_NewFrame();
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();
            ui.draw();
            done = ui.quitRequested();
            ImGui::Render();
            SDL_SetRenderScale(renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
            SDL_SetRenderDrawColor(renderer, 0x0A, 0x14, 0x1E, 0xFF);
            SDL_RenderClear(renderer);
            ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
            SDL_RenderPresent(renderer);
            if (frames > 0) {
                --frames;
            }
            if (!ImGui::GetCurrentContext()->InputEventsQueue.empty()) {
                frames = std::max(frames, 1);
            }
        }
        if (!ui.shutDown()) {
            std::_Exit(0);
        }
    }

    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}

}

int main(int, char**) {
#ifdef _WIN32
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);
#endif
    libssh2_init(0);
    int code = run();
    libssh2_exit();
#ifdef _WIN32
    WSACleanup();
#endif
    return code;
}
