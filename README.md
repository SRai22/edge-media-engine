# edge-media-engine

A small C++23 media runtime: capture once, encode once per rendition, and fan out to many consumers (WebRTC P2P, SFU)
over bounded, keyframe-aware queues. See [PLAN.md](PLAN.md) for the full specification, [docs/architecture.md](docs/architecture.md)
for what is built, and [tickets/](tickets/) for the roadmap.

**Status:** Milestone 1 (media core) and direct WebRTC peers (EME-008), on synthetic media with OpenH264. Camera capture on Linux (V4L2) and Windows (Media Foundation), with automatic resume after unplug/replug; Windows builds and passes tests in CI. Hardware H.264 through Media Foundation (Intel Quick Sync) is written and waiting for its device run; the production SFU publisher is not built yet.

## Try it: every camera on this machine in the browser

```bash
./build-vcpkg/camera_viewer            # add --test-pattern for an extra synthetic camera
```
Open the printed `http://127.0.0.1:8640/?token=…` URL. Each camera is encoded once with H.264; every extra browser tab is
another WebRTC viewer of the same encoded streams (the page shows the viewer count and per-camera encoded frames).
Options: `--size 1280x720 --fps 15 --bitrate 1200000 --port 8640 --max-viewers 4 --listen 127.0.0.1`. On Linux your user must be
in the `video` group. Unplugging a camera pauses its tile; plugging it back in resumes the stream automatically.
`camera_probe` prints each camera's capture modes as JSON. On Windows run `build-win\Release\camera_viewer.exe` (build steps below). On Windows it encodes with the hardware encoder when one is available and falls back to OpenH264; `--encoder mf` or `--encoder openh264` forces one.

## Build: core only (Linux, system packages)

```bash
sudo apt install cmake g++ libspdlog-dev libgtest-dev nlohmann-json3-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build -j
(cd build && ctest --output-on-failure)
./build/synthetic_streamer 5
./build/synthetic_streamer 5 --config configs/synthetic_streamer.json   # topology from JSON
```
Add `-DEME_WITH_WEBRTC=OFF -DEME_WITH_OPENH264=OFF -DEME_WITH_CAMERA=OFF` if the vcpkg dependencies below are not installed
(and `-DEME_WITH_CONFIG=OFF` without nlohmann/json, which disables `--config`).

`configs/synthetic_streamer.json` documents the config format: a list of pipelines, each one source → one encoder → N sinks.
Every field is checked when the file loads, and errors name the field, e.g. `pipelines[1].encoder.bitrateBps: must be in [10000, 100000000]`.
Unknown fields are rejected and missing ones take their defaults.

## Build: full (vcpkg: libdatachannel, OpenH264, nlohmann/json)

`vcpkg.json` pins the baseline. On Linux, OpenH264 needs `nasm` on `PATH`.

```bash
cmake -S . -B build-vcpkg -DCMAKE_TOOLCHAIN_FILE=$HOME/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build-vcpkg -j && (cd build-vcpkg && ctest --output-on-failure)
EME_TOKEN=$(python3 -c 'import secrets;print(secrets.token_urlsafe(16))') ./build-vcpkg/webrtc_streamer 8443
```
Serve `apps/webrtc_streamer/` (e.g. `python3 -m http.server 8000 --directory apps/webrtc_streamer`) and open
`http://localhost:8000/viewer.html?port=8443&token=<EME_TOKEN>`.

## Build: Windows (MSVC + vcpkg)

Needs Visual Studio 2022 (Desktop C++ workload) and Git. In a *Developer PowerShell*:

```powershell
git clone https://github.com/microsoft/vcpkg $HOME\vcpkg
git -C $HOME\vcpkg checkout (Get-Content vcpkg.json | ConvertFrom-Json).'builtin-baseline'
& $HOME\vcpkg\bootstrap-vcpkg.bat -disableMetrics
cmake -S . -B build-win -A x64 -DCMAKE_TOOLCHAIN_FILE=$HOME\vcpkg\scripts\buildsystems\vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows-static-md
cmake --build build-win --config Release --parallel
ctest --test-dir build-win -C Release --output-on-failure
build-win\Release\camera_probe.exe
build-win\Release\camera_viewer.exe
```

Use the static triplet: with plain `x64-windows`, libdatachannel's DLL import library clashes with our `std::vector<std::byte>` instantiations at link time (LNK2005).

The camera backend is Media Foundation (`src/sources/windows_mf/`). Its DLLs are delay-loaded: on systems without
Media Foundation (Windows N, Server without the feature, IoT images with the package removed) the apps run and report
no cameras. Allow desktop apps to use the camera under *Settings → Privacy → Camera*. CI: `.github/workflows/windows.yml`.

Sanitizers: add `-DCMAKE_CXX_FLAGS="-fsanitize=thread"`. On Linux kernels ≥ 6.6, run TSan binaries under `setarch -R`
(high ASLR entropy breaks TSan's memory layout), with `TSAN_OPTIONS=suppressions=tests/tsan.supp` for the vcpkg build.
