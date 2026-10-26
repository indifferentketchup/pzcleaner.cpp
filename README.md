# pzcleaner.cpp

A C++ desktop app version of [pzcleaner](https://pzcleaner.indifferentketchup.com).
It resets parts of a Project Zomboid Build 42 map, either in a save on a
game server over SFTP or in a save folder on the computer it runs on. It is
a C++ port of [pzcleaner](https://github.com/indifferentketchup/pzcleaner)
(Go) with the same features and speed.

The window is drawn natively with Dear ImGui on SDL3, with no browser
engine inside. It has the same steps as the website: the map of saved
chunks, delete and keep boxes, protections and options. Indexing the chunk
files, deleting, restoring and emptying the trash run on background threads,
listing up to 256 folders and moving or deleting up to 512 files at once.

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

- Linux: any X11 or Wayland desktop. The AppImage only needs the system C
  library; SDL loads X11 or Wayland and OpenGL at run time. The folder
  picker uses the desktop portal (or zenity); the folder path can also be
  typed in.
- Windows 10 and 11.
- macOS 11 or newer.

If the window stays black on a machine with broken graphics drivers, start
it with `SDL_RENDER_DRIVER=software`.

## Updates

On start, pzcleaner asks GitHub for the latest release. If it is newer, a
bar offers Update, What's new and Later; nothing is downloaded until Update
is pressed. Update downloads this platform's file from the release, checks
it against the release's `SHA256SUMS.txt` and puts it in place of the
running copy (the AppImage file, the `.exe`, or `pzcleaner.app`), then
offers to restart. A copy that can't replace itself, for example one in a
folder it can't write to or a Mac app run straight from Downloads, links to
the release page instead. Builds without a release version (`dev`, the
default for local builds) never check. CI stamps the version from the tag
with `-DPZCLEANER_VERSION`.

It logs in with the SFTP host, port, user and password from the server
host's panel; no SSH keys are needed. The first connection to a server shows
its host key fingerprint and remembers it once trusted, in
`pzcleaner/known_hosts` under the user config folder. The password is never
stored. The last login (host, port, user and save path), the last local
folder and the keep boxes are kept in `pzcleaner/settings.json` in the same
folder. SSH runs on libssh2 with mbedTLS, which covers RSA and ECDSA host
keys but not Ed25519; OpenSSH servers offer both, so this only matters for a
server that has nothing but an Ed25519 key.

## How it is built

- `src/sftp.cpp`: the SSH connection (libssh2) and a small SFTP v3 client on
  one channel that keeps many requests in flight, since libssh2's own SFTP
  API handles one request at a time per channel.
- `src/store.cpp`: the same file operations over SFTP or the local disk.
- `src/app.cpp`: open, index, delete, restore and the `vehicles.db` edit
  (SQLite).
- `src/save.cpp`: reading a save (`map_meta.bin` safehouses, `players.db`,
  `vehicles.db`) and working out which files a selection deletes. It is a
  port of the website's `js/save.js`, `js/meta.js` and `js/db.js`, so keep
  the two in step.
- `src/mapview.cpp`: the map, with pan, zoom and box drawing.
- `src/ui.cpp` and `src/main.cpp`: the window, the open dialog and the
  steps.
- `src/update.cpp`: the update check and install (cpp-httplib over mbedTLS,
  trusting the system's certificate store).

CMake fetches every dependency as a pinned, hash-checked source archive:
SDL 3.4.18, Dear ImGui 1.93.0, mbedTLS 3.6.7, libssh2 1.11.1, cpp-httplib
0.63.0, nlohmann/json 3.12.0 and SQLite 3.54.1. The font is Roboto Medium from Dear ImGui's
`misc/fonts`.

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Linux needs CMake 3.24+, a C++20 compiler, Ninja and the X11, Wayland,
EGL/OpenGL and D-Bus development headers (the package list is in
`.github/workflows/release.yml`). Windows builds with Visual Studio 2022,
macOS with Xcode's command line tools.
