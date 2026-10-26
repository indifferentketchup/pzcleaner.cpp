# pzcleaner.cpp

A C++ desktop app version of [pzcleaner](https://pzcleaner.indifferentketchup.com).
It resets parts of a Project Zomboid Build 42 map, either in a save on a
game server over SFTP or in a save folder on the computer it runs on. It is
a C++ port of [pzcleaner](https://github.com/indifferentketchup/pzcleaner)
(Go) with the same features and speed.

The window shows the pzcleaner page itself: the same map, delete and keep
boxes, protections and options. The page is served from inside the app on
`127.0.0.1` and talks to the native side through webview bindings
(`site/desktop.js`). Indexing the chunk files, deleting, restoring and
emptying the trash run natively, listing up to 256 folders and moving or
deleting up to 512 files at once.

Deleted files go to `pzcleaner-backup/<save name>/<date_time>/` in the
Zomboid folder, beside `Saves`, unless "Delete for good (faster)" is ticked.
Stop the game server before deleting.

## Download

Builds are on the releases page: `pzcleaner-windows-amd64.exe`,
`pzcleaner-x86_64.AppImage` for Linux (make it executable, then run it) and
`pzcleaner-macos.zip` (Intel and Apple silicon, unsigned: right-click the
app and choose Open the first time). `SHA256SUMS.txt` lists each file's
checksum. Pushing a `v*` tag builds them with GitHub Actions
(`.github/workflows/release.yml`).

## Run

- Linux: needs WebKitGTK 4.1 (`webkit2gtk-4.1` on Arch and CachyOS,
  `libwebkit2gtk-4.1-0` on Debian and Ubuntu).
- Windows 10 and 11: needs the WebView2 runtime, which ships with them.
- macOS 11 or newer.

It logs in with the SFTP host, port, user and password from the server
host's panel; no SSH keys are needed. The first connection to a server shows
its host key fingerprint and remembers it once trusted, in
`pzcleaner/known_hosts` under the user config folder. The password is never
stored. SSH runs on libssh2 with mbedTLS, which covers RSA and ECDSA host
keys but not Ed25519; OpenSSH servers offer both, so this only matters for a
server that has nothing but an Ed25519 key.

## How it is built

- `src/sftp.cpp`: the SSH connection (libssh2) and a small SFTP v3 client on
  one channel that keeps many requests in flight, since libssh2's own SFTP
  API handles one request at a time per channel.
- `src/store.cpp`: the same file operations over SFTP or the local disk.
- `src/app.cpp`: open, index, delete, restore and the `vehicles.db` edit
  (SQLite).
- `src/main.cpp`: the window (webview), the built-in page server
  (cpp-httplib), the folder picker (nativefiledialog-extended) and the
  bindings.

CMake fetches every dependency as a pinned, hash-checked source archive:
mbedTLS 3.6.7, libssh2 1.11.1, webview 0.12.0, nlohmann/json 3.12.0,
cpp-httplib 0.63.0, nativefiledialog-extended 1.4.1 and SQLite 3.54.1.

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Linux needs CMake 3.24+, a C++20 compiler, Ninja, `libgtk-3-dev` and
`libwebkit2gtk-4.1-dev`. Windows builds with Visual Studio 2022, macOS with
Xcode's command line tools.

## Updating the page

`site/` is a copy of the pzcleaner site, apart from `site/desktop.js`.
Refresh it with:

```bash
./sync-site.sh /opt/ib-gametools/projectzomboid/pzchunkcleaner
```

The script adds the `desktop.js` script tag, rewords the page where it talks
about the browser or the trash location, and points `scanChunks`,
`executePlan` and `restoreTrash` in `js/save.js` at the native side. It
stops with an error if those no longer match.
