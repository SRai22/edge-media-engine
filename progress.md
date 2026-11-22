# Progress

**Task:** Implement PLAN.md (edge media engine) incrementally with the loop workflow; long-term work as `tickets/EME-NNN-*.md`;
prefer established OSS libraries over custom code; subagents on Sonnet/Haiku.
**Constraint:** This environment is Linux with no Windows device, cameras or Intel encoder. Hardware milestones are blocked (PLAN §24.2).

## Outputs
- Code: `CMakeLists.txt`, `vcpkg.json`, `include/eme/**`, `src/**`, `tests/**`, `apps/synthetic_streamer/main.cpp`
- Docs: `README.md`, `docs/architecture.md`, `docs/design-decisions/ADR-00{1,2,3}-*.md`
- Tickets: `tickets/EME-001` … `EME-013`
- EME-008: `include/eme/transport/webrtc_peer_server.hpp`, `src/transport/webrtc_p2p/`, `include/eme/processing/openh264_encoder.hpp`,
  `src/processing/encoder/`, `apps/webrtc_streamer/{main.cpp,viewer.html}`, `docs/design-decisions/ADR-004-*.md`, `tests/tsan.supp`,
  `.claude/launch.json` (serves the viewer for browser checks)
- EME-014/015/016: `include/eme/transport/whip_publisher.hpp`, `src/transport/whip/`, `src/transport/webrtc_common/`,
  `apps/spikes/**`, `spikes/livekit/` (local LiveKit stack: `up.sh`/`down.sh`), `spikes/sfu_eval/` (harness),
  `docs/reports/sfu-publisher-spike.md` (+ `data/`), `docs/design-decisions/ADR-005-*.md` (proposed)
- EME-017: `include/eme/sources/camera.hpp`, `src/sources/v4l2/camera_v4l2.cpp`, `apps/camera_probe/`, `apps/camera_viewer/`
  (`main.cpp`, `viewer.html` embedded at build), `tests/integration/camera_test.cpp`
- Build trees: `build/` (core only), `build-vcpkg/` (+ `-addr`, `-thre` sanitizer trees), `build-spike/` (Release, spikes; use a fresh tree when switching build type: pkg-config caches debug OpenH264)
- Verification: `checks.md` · task notes: `sources/task.md`

## Completed
- **EME-002 / Milestone 1 (media core):** done on synthetic media. 23/23 tests pass on plain, ASan+UBSan and TSan builds
  (repeated 10×). The demo runs 2 sources → 2 encoders → 3 sinks each, with slow sinks dropping and recovering and 0 dependency violations.
- Independent review: 4 of 6 findings fixed, 2 ticketed (checks.md).
- **EME-008 direct WebRTC peers:** done for P2P. 32/32 tests (plain, ASan+UBSan, TSan). Chrome decodes both 720p15
  tracks with 0 loss. Sonnet review: 7 findings, 6 fixed, 1 accepted. OpenH264 software encoder added as fallback.
- **EME-014/015/016 SFU publisher spikes:** both paths work against local LiveKit. Measured over 3 runs each: equal CPU and
  latency; WHIP has faster late-join recovery (85 vs ~230 ms), 25 MB less RSS and 32 MB less footprint. WHIP is recommended (ADR-005, proposed).

## Decisions
- Custom bounded keyframe-aware queue: no library offers flush-and-wait-for-keyframe semantics (ADR-002).
- Keyframe requests are rate-limited per encoder (default 500 ms) after the demo showed one slow viewer inflating keyframes 10 → 7 in 3 s.
- Static typed DAG with `Producer<T>`/`Consumer<T>` ports instead of a plugin/caps system (ADR-003).
- Test doubles live in the `eme::mock` namespace (`eme::testing` collides with GoogleTest).
- libdatachannel + OpenH264 via a pinned vcpkg manifest. libx264 rejected (GPL). LiveKit's SDK is not needed for P2P (ADR-004).

## Open issues
- Windows/MSVC build and vcpkg manifest unverified (EME-012).
- JSON config not implemented. nlohmann/json is now a dependency (EME-003).
- Hardware work (EME-001, 004–006) needs the target device.
- Not tested: WebRTC under real loss (netem), SFU + P2P together, bitrate on real content.
- `~/vcpkg` and `~/.local/bin/nasm` were installed outside the repo during this session.

- **EME-017 Linux camera example:** `camera_viewer` streams every camera (here: integrated + Logitech USB + optional test
  pattern) to the browser, each encoded once. Verified live with 2 tabs (40 frames/s encoded, 80 frames/s sent). 40/40 tests under
  ASan/TSan. Sonnet review: 8 findings, 7 fixed. Branch `feat/eme-017-linux-camera-example` is stacked on the spike branch.
- Owner decisions pending: ADR-005 (WHIP vs SDK). Also, the merge to main (30f74d6) left out `tickets/` and the
  ADR-001..004 files; they exist only in the working tree / this branch.
- Installed outside the repo: `~/deps/livekit-sdk` (prebuilt SDK, digest-verified), `~/deps/lk` (lk CLI 2.19.2), and Docker images
  livekit-server v1.13.11, ingress v1.6.0, redis 7.4.

- **EME-004 Windows camera (Media Foundation):** `src/sources/windows_mf/camera_mf.cpp` implements `camera.hpp`; shared
  helpers in `src/sources/camera_common.hpp` (Linux backend now uses them, 42/42). Windows CI workflow `.github/workflows/windows.yml`.
  Sonnet review: 12 findings, 7 fixed. MF DLLs are delay-loaded so apps run without Media Foundation. Not compiled yet: CI runs on push.

## Next action
EME-004: push the branch (owner approval) so Windows CI compiles it; fix any MSVC errors/warnings it reports; update checks.md.
Then owner hardware steps on the Windows box (see README "Build: Windows"): `camera_probe.exe` lists both cameras with modes;
`ctest` with a camera attached (capture + simulated-loss tests run, not skipped); `camera_viewer.exe` shows both cameras at 720p15;
unplug one during capture → its tile pauses, the other continues; replug (same and another port) → resumes, `reconnects` = 1.
Once ADR-005 is accepted: productionize `WhipPublisher` under EME-007. Otherwise EME-003 (JSON config).
