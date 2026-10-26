cmake_minimum_required(VERSION 3.24)
project(pzcleaner LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

include(FetchContent)
set(FETCHCONTENT_QUIET ON)

FetchContent_Declare(mbedtls
    URL https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.7/mbedtls-3.6.7.tar.bz2
    URL_HASH SHA256=a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6)
FetchContent_Declare(libssh2
    URL https://github.com/libssh2/libssh2/releases/download/libssh2-1.11.1/libssh2-1.11.1.tar.gz
    URL_HASH SHA256=d9ec76cbe34db98eec3539fe2c899d26b0c837cb3eb466a56b0f109cabf658f7)
FetchContent_Declare(webview
    URL https://github.com/webview/webview/archive/refs/tags/0.12.0.tar.gz
    URL_HASH SHA256=e2c8d0bed3fcd13e624074448b7d3b1d0b0d03546a15839f17ee0013f2cba1db)
FetchContent_Declare(json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa)
FetchContent_Declare(httplib
    URL https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.63.0.tar.gz
    URL_HASH SHA256=44ab42e16926e95f7641d286bc90d739ab1c16886114d7cc5bc7a9f905c5edf8)
FetchContent_Declare(nfd
    URL https://github.com/btzy/nativefiledialog-extended/archive/refs/tags/v1.4.1.tar.gz
    URL_HASH SHA256=4a01eafa921169f009f3e83bea8e287d9e5d0b94ca9064cf7f0a546652d7c884)
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

set(WEBVIEW_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(WEBVIEW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WEBVIEW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WEBVIEW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(WEBVIEW_ENABLE_CHECKS OFF CACHE BOOL "" FORCE)
set(WEBVIEW_ENABLE_PACKAGING OFF CACHE BOOL "" FORCE)
set(WEBVIEW_INSTALL_TARGETS OFF CACHE BOOL "" FORCE)
set(WEBVIEW_USE_STATIC_MSVC_RUNTIME ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(webview)

set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(json)

set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZLIB_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_BROTLI_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZSTD_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(httplib)

set(NFD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(NFD_WAYLAND OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(nfd)

FetchContent_MakeAvailable(sqlite)
add_library(sqlite3 STATIC ${sqlite_SOURCE_DIR}/sqlite3.c)
target_include_directories(sqlite3 PUBLIC ${sqlite_SOURCE_DIR})
target_compile_definitions(sqlite3 PRIVATE SQLITE_OMIT_LOAD_EXTENSION SQLITE_THREADSAFE=1)

add_executable(embed tools/embed.cpp)
file(GLOB_RECURSE SITE_FILES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/site/*)
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/assets.cpp
    COMMAND embed ${CMAKE_BINARY_DIR}/assets.cpp ${CMAKE_SOURCE_DIR}/site ${SITE_FILES}
    DEPENDS embed ${SITE_FILES})

add_executable(pzcleaner WIN32
    src/main.cpp
    src/app.cpp
    src/sftp.cpp
    src/store.cpp
    ${CMAKE_BINARY_DIR}/assets.cpp)
target_include_directories(pzcleaner PRIVATE src)
if(NOT MSVC)
    target_compile_options(pzcleaner PRIVATE -Wall -Wextra)
endif()
target_link_libraries(pzcleaner PRIVATE
    webview::core_static
    libssh2::libssh2_static
    nlohmann_json::nlohmann_json
    httplib::httplib
    nfd
    sqlite3)
if(WIN32)
    target_compile_definitions(pzcleaner PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(pzcleaner PRIVATE ws2_32)
elseif(APPLE)
    target_link_libraries(pzcleaner PRIVATE "-framework AppKit")
else()
    target_link_options(pzcleaner PRIVATE -static-libstdc++ -static-libgcc)
endif()
