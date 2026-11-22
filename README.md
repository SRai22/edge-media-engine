# edge-media-engine

A small C++23 media runtime: capture once, encode once per rendition, and fan out to many consumers (WebRTC P2P, SFU)
over bounded, keyframe-aware queues. See [PLAN.md](PLAN.md) for the full specification, [docs/architecture.md](docs/architecture.md)
for what is built, and [tickets/](tickets/) for the roadmap.

**Status:** Milestone 1 (media core on synthetic media). Windows capture, hardware encoding and WebRTC are not built yet.

## Build (Linux, system packages)

```bash
sudo apt install cmake g++ libspdlog-dev libgtest-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build -j
(cd build && ctest --output-on-failure)
./build/synthetic_streamer 5
```

Windows/vcpkg: `vcpkg.json` lists the dependencies. It is not yet verified on Windows (EME-012).

Sanitizers: add `-DCMAKE_CXX_FLAGS="-fsanitize=thread"`. On Linux kernels ≥ 6.6, run TSan binaries under `setarch -R`
(high ASLR entropy breaks TSan's memory layout).
