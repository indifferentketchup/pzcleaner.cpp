cmake_minimum_required(VERSION 3.24)
project(pzcleaner LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(PZCLEANER_VERSION "dev" CACHE STRING "Release version, without the v; dev builds never update themselves")

include(FetchContent)
set(FETCHCONTENT_QUIET ON)

FetchContent_Declare(mbedtls
    URL https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.7/mbedtls-3.6.7.tar.bz2
    URL_HASH SHA256=a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6)
FetchContent_Declare(libssh2
    URL https://github.com/libssh2/libssh2/releases/download/libssh2-1.11.1/libssh2-1.11.1.tar.gz
    URL_HASH SHA256=d9ec76cbe34db98eec3539fe2c899d26b0c837cb3eb466a56b0f109cabf658f7)
FetchContent_Declare(sdl
    URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.tar.gz
    URL_HASH SHA256=9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3)
FetchContent_Declare(imgui
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.93.0.tar.gz
    URL_HASH SHA256=f7bf6bc76e5d79d40d751096663e6c5d18daf08129dd52e7f7b09e4251c8eeee)
FetchContent_Declare(json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa)
FetchContent_Declare(httplib
    URL https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.63.0.tar.gz
    URL_HASH SHA256=44ab42e16926e95f7641d286bc90d739ab1c16886114d7cc5bc7a9f905c5edf8
    SOURCE_SUBDIR header-only)
FetchContent_Declare(sqlite
    URL https://www.sqlite.org/2026/sqlite-amalgamation-3540100.zip
    URL_HASH SHA256=0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9)

set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
set(DISABLE_PACKAGE_CONFIG_AND_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(mbedtls)

set(CRYPTO_BACKEND mbedTLS CACHE STRING "" FORCE)
set(MBEDTLS_INCLUDE_DIR ${mbedtls_SOURCE_DIR}/include CACHE PATH "" FORCE)
set(MBEDCRYPTO_LIBRARY mbedcrypto CACHE STRING "" FORCE)
set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(ENABLE_ZLIB_COMPRESSION OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(libssh2)

set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
foreach(subsystem AUDIO GPU CAMERA JOYSTICK HAPTIC HIDAPI POWER SENSOR TRAY)
    set(SDL_${subsystem} OFF CACHE BOOL "" FORCE)
endforeach()
set(SDL_VULKAN OFF CACHE BOOL "" FORCE)
set(SDL_KMSDRM OFF CACHE BOOL "" FORCE)
set(SDL_PIPEWIRE OFF CACHE BOOL "" FORCE)
set(SDL_LIBUDEV OFF CACHE BOOL "" FORCE)
set(SDL_LIBURING OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(sdl)

FetchContent_MakeAvailable(imgui)
add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends ${imgui_SOURCE_DIR}/misc/cpp)
target_link_libraries(imgui PUBLIC SDL3::SDL3-static)

set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(json)

FetchContent_MakeAvailable(httplib)

FetchContent_MakeAvailable(sqlite)
add_library(sqlite3 STATIC ${sqlite_SOURCE_DIR}/sqlite3.c)
target_include_directories(sqlite3 PUBLIC ${sqlite_SOURCE_DIR})
target_compile_definitions(sqlite3 PRIVATE SQLITE_OMIT_LOAD_EXTENSION SQLITE_THREADSAFE=1)

add_executable(embed tools/embed.cpp)
set(ASSET_FILES
    ${CMAKE_SOURCE_DIR}/assets/worldmap.png
    ${CMAKE_SOURCE_DIR}/assets/broccoli-mark.png
    ${imgui_SOURCE_DIR}/misc/fonts/Roboto-Medium.ttf)
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/assets.cpp
    COMMAND embed ${CMAKE_BINARY_DIR}/assets.cpp ${ASSET_FILES}
    DEPENDS embed ${ASSET_FILES})

add_executable(pzcleaner WIN32
    src/main.cpp
    src/ui.cpp
    src/mapview.cpp
    src/save.cpp
    src/app.cpp
    src/sftp.cpp
    src/store.cpp
    src/update.cpp
    ${CMAKE_BINARY_DIR}/assets.cpp)
target_include_directories(pzcleaner PRIVATE src ${httplib_SOURCE_DIR})
target_compile_definitions(pzcleaner PRIVATE PZCLEANER_VERSION="${PZCLEANER_VERSION}" CPPHTTPLIB_MBEDTLS_SUPPORT)
if(NOT MSVC)
    target_compile_options(pzcleaner PRIVATE -Wall -Wextra)
endif()
target_link_libraries(pzcleaner PRIVATE
    imgui
    SDL3::SDL3-static
    libssh2::libssh2_static
    mbedtls
    mbedx509
    mbedcrypto
    nlohmann_json::nlohmann_json
    sqlite3)
if(WIN32)
    target_compile_definitions(pzcleaner PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(pzcleaner PRIVATE ws2_32 crypt32)
elseif(APPLE)
    target_link_libraries(pzcleaner PRIVATE "-framework CoreFoundation" "-framework Security")
else()
    target_link_options(pzcleaner PRIVATE -static-libstdc++ -static-libgcc)
endif()
