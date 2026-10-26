#include "app.hpp"
#include "assets.hpp"

#include <httplib.h>
#include <libssh2.h>
#include <nfd.h>
#include <webview/webview.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>
#include <unordered_map>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include <spawn.h>
#include <sys/wait.h>
extern char** environ;
#endif

#ifdef __APPLE__
#include <objc/message.h>
#include <objc/runtime.h>
#endif

using nlohmann::json;

namespace {

std::atomic<int> activeCalls{0};

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

std::string mimeType(std::string_view path) {
    static const std::unordered_map<std::string_view, const char*> kTypes = {
        {".html", "text/html; charset=utf-8"}, {".js", "text/javascript; charset=utf-8"},
        {".css", "text/css; charset=utf-8"},   {".png", "image/png"},
        {".jpg", "image/jpeg"},                {".svg", "image/svg+xml"},
        {".wasm", "application/wasm"},         {".json", "application/json"},
        {".txt", "text/plain; charset=utf-8"}, {".xml", "application/xml"},
    };
    std::size_t dot = path.find_last_of('.');
    if (dot != std::string_view::npos) {
        auto it = kTypes.find(path.substr(dot));
        if (it != kTypes.end()) {
            return it->second;
        }
    }
    return "application/octet-stream";
}

std::string errorText(const std::exception& error) {
    if (const auto* named = dynamic_cast<const NamedError*>(&error)) {
        return named->name + ": " + named->what();
    }
    if (const auto* fsError = dynamic_cast<const FsError*>(&error)) {
        return std::string(fsError->notFound ? "NotFoundError: " : "Error: ") + fsError->what();
    }
    return std::string("Error: ") + error.what();
}

void bindAsync(webview::webview& view, const std::string& name, std::function<json(const json&)> handler) {
    view.bind(
        name,
        [&view, handler](const std::string& id, const std::string& request, void*) {
            ++activeCalls;
            std::thread([&view, handler, id, request] {
                try {
                    json result = handler(json::parse(request));
                    view.resolve(id, 0, result.dump());
                } catch (const std::exception& error) {
                    view.resolve(id, 1, json(errorText(error)).dump());
                }
                --activeCalls;
            }).detach();
        },
        nullptr);
}

void openInBrowser(const std::string& url) {
    if (url.rfind("https://", 0) != 0 && url.rfind("http://", 0) != 0) {
        return;
    }
#ifdef _WIN32
    ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
#ifdef __APPLE__
    const char* program = "open";
#else
    const char* program = "xdg-open";
#endif
    char* argv[] = {const_cast<char*>(program), const_cast<char*>(url.c_str()), nullptr};
    pid_t pid = 0;
    if (posix_spawnp(&pid, program, nullptr, nullptr, argv, environ) == 0) {
        std::thread([pid] {
            int status = 0;
            waitpid(pid, &status, 0);
        }).detach();
    }
#endif
}

#ifdef __APPLE__
id objcClass(const char* name) { return reinterpret_cast<id>(objc_getClass(name)); }

template <typename Result = id, typename... Args>
Result objcSend(id target, const char* selector, Args... args) {
    return reinterpret_cast<Result (*)(id, SEL, Args...)>(objc_msgSend)(target, sel_registerName(selector), args...);
}

id nsString(const char* text) { return objcSend(objcClass("NSString"), "stringWithUTF8String:", text); }

id menuItem(const char* title, const char* action, const char* key) {
    id item = objcSend(objcClass("NSMenuItem"), "alloc");
    return objcSend(item, "initWithTitle:action:keyEquivalent:", nsString(title), sel_registerName(action), nsString(key));
}

id submenu(id bar, const char* title) {
    id holder = objcSend(objcSend(objcClass("NSMenuItem"), "alloc"), "init");
    objcSend<void>(bar, "addItem:", holder);
    id menu = objcSend(objcSend(objcClass("NSMenu"), "alloc"), "initWithTitle:", nsString(title));
    objcSend<void>(holder, "setSubmenu:", menu);
    return menu;
}

void addMacMenus() {
    id bar = objcSend(objcSend(objcClass("NSMenu"), "alloc"), "init");
    id appMenu = submenu(bar, "pzcleaner");
    objcSend<void>(appMenu, "addItem:", menuItem("Quit pzcleaner", "terminate:", "q"));
    id edit = submenu(bar, "Edit");
    objcSend<void>(edit, "addItem:", menuItem("Undo", "undo:", "z"));
    objcSend<void>(edit, "addItem:", menuItem("Redo", "redo:", "Z"));
    objcSend<void>(edit, "addItem:", menuItem("Cut", "cut:", "x"));
    objcSend<void>(edit, "addItem:", menuItem("Copy", "copy:", "c"));
    objcSend<void>(edit, "addItem:", menuItem("Paste", "paste:", "v"));
    objcSend<void>(edit, "addItem:", menuItem("Select All", "selectAll:", "a"));
    objcSend<void>(objcSend(objcClass("NSApplication"), "sharedApplication"), "setMainMenu:", bar);
}
#endif

int run() {
    std::unordered_map<std::string_view, const Asset*> assets;
    for (std::size_t i = 0; i < kAssetCount; ++i) {
        assets.emplace(kAssets[i].path, &kAssets[i]);
    }
    httplib::Server server;
    server.Get(R"(/.*)", [&assets](const httplib::Request& request, httplib::Response& response) {
        std::string path = request.path == "/" ? "/index.html" : request.path;
        auto it = assets.find(path);
        if (it == assets.end()) {
            response.status = 404;
            return;
        }
        response.set_header("Cache-Control", "no-store");
        response.set_content(reinterpret_cast<const char*>(it->second->data), it->second->size, mimeType(path));
    });
    int port = server.bind_to_any_port("127.0.0.1");
    if (port <= 0) {
        return 1;
    }
    std::thread serverThread([&server] { server.listen_after_bind(); });

    NFD_Init();
    webview::webview view(false, nullptr);
    view.set_title("pzcleaner");
    view.set_size(960, 640, WEBVIEW_HINT_MIN);
    view.set_size(1400, 900, WEBVIEW_HINT_NONE);
#ifdef __APPLE__
    addMacMenus();
#endif

    App app(configDir() + "/known_hosts", [&view](std::int64_t done, std::int64_t total) {
        view.dispatch([&view, done, total] {
            view.eval("window.pzProgress && window.pzProgress(" + std::to_string(done) + "," + std::to_string(total) +
                      ")");
        });
    });

    bindAsync(view, "pzConnect", [&app](const json& args) { return app.connect(args.at(0)); });
    bindAsync(view, "pzList", [&app](const json& args) { return app.list(args.at(0)); });
    bindAsync(view, "pzStat", [&app](const json& args) { return app.stat(args.at(0)); });
    bindAsync(view, "pzRead", [&app](const json& args) { return json(app.read(args.at(0))); });
    bindAsync(view, "pzWrite", [&app](const json& args) {
        app.write(args.at(0), args.at(1));
        return json(nullptr);
    });
    bindAsync(view, "pzMkdir", [&app](const json& args) {
        app.mkdir(args.at(0));
        return json(nullptr);
    });
    bindAsync(view, "pzRename", [&app](const json& args) {
        app.rename(args.at(0), args.at(1));
        return json(nullptr);
    });
    bindAsync(view, "pzRemove", [&app](const json& args) {
        app.remove(args.at(0), args.at(1).get<bool>());
        return json(nullptr);
    });
    bindAsync(view, "pzIndex", [&app](const json& args) { return app.index(args.at(0)); });
    bindAsync(view, "pzExecute", [&app](const json& args) { return app.execute(args.at(0)); });
    bindAsync(view, "pzRestore", [&app](const json& args) { return json(app.restore(args.at(0))); });
    view.bind(
        "pzOpenLocal",
        [&view, &app](const std::string& id, const std::string&, void*) {
            nfdu8char_t* picked = nullptr;
            nfdresult_t result = NFD_PickFolderU8(&picked, nullptr);
            if (result == NFD_CANCEL) {
                view.resolve(id, 0, json::object().dump());
                return;
            }
            if (result != NFD_OKAY) {
                view.resolve(id, 1, json(std::string("Error: ") + NFD_GetError()).dump());
                return;
            }
            std::string dir(reinterpret_cast<const char*>(picked));
            NFD_FreePathU8(picked);
            try {
                view.resolve(id, 0, app.openLocal(dir).dump());
            } catch (const std::exception& error) {
                view.resolve(id, 1, json(errorText(error)).dump());
            }
        },
        nullptr);
    view.bind(
        "pzOpenURL",
        [&view](const std::string& id, const std::string& request, void*) {
            json args = json::parse(request);
            if (!args.empty() && args[0].is_string()) {
                openInBrowser(args[0].get<std::string>());
            }
            view.resolve(id, 0, "null");
        },
        nullptr);

    view.navigate("http://127.0.0.1:" + std::to_string(port) + "/");
    view.run();

    app.disconnect();
    for (int i = 0; i < 100 && activeCalls > 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    server.stop();
    serverThread.join();
    NFD_Quit();
    if (activeCalls > 0) {
        std::_Exit(0);
    }
    return 0;
}

}

#ifdef _WIN32
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);
    libssh2_init(0);
    int code = run();
    libssh2_exit();
    WSACleanup();
    return code;
}
#else
int main() {
    libssh2_init(0);
    int code = run();
    libssh2_exit();
    return code;
}
#endif
