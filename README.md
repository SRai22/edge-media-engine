# edge-media-engine

A small C++23 media runtime: capture once, encode once per rendition, and fan out to many consumers (WebRTC P2P, SFU)
over bounded, keyframe-aware queues. See [PLAN.md](PLAN.md) for the full specification, [docs/architecture.md](docs/architecture.md)
for what is built, and [tickets/](tickets/) for the roadmap.

**Status:** Milestone 1 (media core) and direct WebRTC peers (EME-008), on synthetic media with OpenH264. Windows capture, hardware encoding and the SFU publisher are not built yet.

## Build: core only (Linux, system packages)

```bash
sudo apt install cmake g++ libspdlog-dev libgtest-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build -j
(cd build && ctest --output-on-failure)
./build/synthetic_streamer 5
```
Add `-DEME_WITH_WEBRTC=OFF -DEME_WITH_OPENH264=OFF` if the vcpkg dependencies below are not installed.

## Build: full (vcpkg: libdatachannel, OpenH264, nlohmann/json)

`vcpkg.json` pins the baseline. On Linux, OpenH264 needs `nasm` on `PATH`.

```bash
cmake -S . -B build-vcpkg -DCMAKE_TOOLCHAIN_FILE=$HOME/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build-vcpkg -j && (cd build-vcpkg && ctest --output-on-failure)
EME_TOKEN=$(python3 -c 'import secrets;print(secrets.token_urlsafe(16))') ./build-vcpkg/webrtc_streamer 8443
```
Serve `apps/webrtc_streamer/` (e.g. `python3 -m http.server 8000 --directory apps/webrtc_streamer`) and open
`http://localhost:8000/viewer.html?port=8443&token=<EME_TOKEN>`.

Windows/vcpkg: `vcpkg.json` lists the dependencies. It is not yet verified on Windows (EME-012).

Sanitizers: add `-DCMAKE_CXX_FLAGS="-fsanitize=thread"`. On Linux kernels ≥ 6.6, run TSan binaries under `setarch -R`
(high ASLR entropy breaks TSan's memory layout), with `TSAN_OPTIONS=suppressions=tests/tsan.supp` for the vcpkg build.
