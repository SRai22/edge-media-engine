# Verification report — EME-002 (Milestone 1)

Review method: automated tests and sanitizers by the main session, plus an independent Sonnet reviewer subagent (read-only).
Environment: Ubuntu 24.04, g++ 13.3, CMake 3.28. No Windows/camera/Intel encoder hardware.

| requirement | verdict | evidence | correction |
|---|---|---|---|
| Compiles as C++23 with `-Wall -Wextra -Wpedantic -Wconversion`, no warnings | pass | `cmake --build build` prints no warnings or errors | fixed GCC nested-default-arg error; renamed `eme::testing`→`eme::mock` (clashed with gtest) |
| Raw + encoded buffer abstractions, immutable, shared without deep copy | pass | `Distributor.FansOutSharedPayloadWithoutCopy`; `FanOut.SharedEncodeToManyConsumers` (same `lastPayload` pointer across sinks) | — |
| Source interface + encoder interface | pass | `include/eme/core/node.hpp`, `include/eme/processing/video_encoder.hpp` | `IMediaSource::capabilities()` deferred to EME-004 (needs real device data) |
| Non-blocking distributor | pass | `FanOut.SharedEncodeToManyConsumers`: a never-read subscription stays at depth ≤ 3 while the others receive every frame | — |
| Static graph: format validation, cycle rejection, lifecycle ordering, rollback | pass | `MediaGraph.*` (4 tests) | — |
| No repeated encoding per consumer (M1 exit) | pass | integration test: `framesIn == encoder input delivered`, 4 subscribers, one encoder | — |
| Bounded memory (M1 exit) | pass | pool `allocated ≤ capacity`; every queue `highWatermark ≤ capacity` | — |
| Consumers never get a P-frame with a missing reference | pass | `dependencyViolations == 0` for fast and slow sinks; `Subscription.*` unit tests | — |
| Keyframe requests coalesced and rate-limited | pass | `MockH264Encoder.CoalescesKeyframeRequests`, `FanOut.KeyframeRequestsAreRateLimited`; demo 10→7 keyframes/3 s | added `minKeyframeInterval` after the demo showed keyframe inflation |
| Thread safety | pass | TSan: 10/10 runs × 23 tests clean after review fixes (20/20 before), `setarch -R ctest` passes; TSan demo: 0 reports | `gtest_discover_tests` set to PRE_TEST so sanitizer builds don't fail at build time |
| Memory safety | pass | ASan+UBSan: `ctest --repeat until-fail:10`, 23/23 | — |
| Basic configuration and logging | partial | spdlog logging in graph/encoder; config is C++ structs only | JSON config → EME-003 (nlohmann/json) |
| Windows build / vcpkg manifest | unresolved | `vcpkg.json` written, not built on Windows | EME-012 |
| Real camera / HW encoder / WebRTC | unresolved | no hardware in this environment | EME-001, 004–008 |
| Independent review findings | pass | Sonnet reviewer: no serious core bugs; 6 findings, 4 fixed, 2 ticketed (below) | see Review |

## Review (Sonnet subagent, read-only, 2026-11-21)
| # | finding | outcome |
|---|---|---|
| 1 | `lastKeyframeForward_` defaulted to steady-clock epoch, so the first keyframe request was suppressed on hosts up for less than the interval (flaky test, real bug) | fixed: `std::optional`, encoder_node.cpp |
| 2 | Integration test's `forcedKeyframes > 0` was satisfied by frame 0, and the assertion `forced < requests+1` was trivially true | fixed: GOP disabled and rate limit set to 0 in that test, so recovery must come from forced keyframes (`slow.keyframes ≥ 2`, `forced ≥ 2`). One new assertion I added (`received > keyframes`) failed because a 10×-overloaded sink with no rate limit only ever gets keyframes. That is expected behaviour, so the assertion was removed and documented in the test |
| 3 | `FailedStartRollsBack` never exercised rollback | fixed: up/down relays, asserts `start down, start up, stop down` |
| 4 | A dead but still-registered subscriber keeps requesting keyframes (~2/s) | ticketed: EME-008 review notes |
| 5 | `start()` after `stop()` reuses stale queue/sink state | ticketed: EME-010 review notes |
| 6 | `emitFrame()` without `start()` on an invalid config: zero-byte memcpy, divide by zero | fixed: `valid()` guard plus test |

---

# Verification report — EME-008 (direct WebRTC peers) · 2026-11-21

Review method: tests and sanitizers in the main session, Chrome in the built-in browser pane, and an independent Sonnet reviewer (read-only).
Dependencies: vcpkg 2026.10.16, libdatachannel 0.24.6, OpenH264 2.6.0, nlohmann-json 3.12.0.

| requirement | verdict | evidence | correction |
|---|---|---|---|
| Use libdatachannel, no custom RTP/RTCP/SRTP | pass | `webrtc_peer_server.cpp` uses H264RtpPacketizer, RtcpSrReporter, RtcpNackResponder, PliHandler. API checked against v0.24.6 headers | — |
| Per-peer RTP/SRTP state, shared encoded payloads | pass | one PeerConnection + SSRC per peer/track; `TwoPeersShareOneEncodePerCamera`: `framesIn ≤ captured` with 2 peers | review: the first version of this assertion was tautological; replaced |
| Peer starts at a decodable keyframe with SPS/PPS | pass | test asserts NAL 7,8,5 first per track per peer | — |
| PLI → coalesced keyframe | pass | `PliFromPeerForcesSharedKeyframe` | — |
| No thread per viewer | pass | one dispatcher thread, woken by the `Subscription` notify hook | — |
| Auth, peer limit, bounded unauthenticated resources | pass | `RejectsBadTokenAndExtraPeers`; constant-time token compare; ≤16 handshakes, 5 s handshake timeout, 15 s answer deadline, 64 KB messages | review: slowloris, a maxPeers race and the missing answer deadline were fixed |
| Untrusted signaling input | pass | `MalformedSignalingClosesPeer` | added after review |
| Disconnected peer releases its queues (M1 review #4) | pass | `DisconnectedPeerReleasesItsQueues` | — |
| Browser decodes stream | pass | Chrome (built-in pane): 2 tracks at 1280×720, 15 fps, H264 42e01f, 0 lost, real-time playback; screenshot shows the moving bar. Re-checked after the review fixes | — |
| OpenH264 output is Constrained Baseline with in-band SPS/PPS | pass | `OpenH264Encoder.ProducesBrowserCompatibleAnnexB` (profile_idc 66, constraint_set1) | SPS profile check added after review |
| Memory safety | pass | ASan+UBSan 3/3 × 32 tests | — |
| Thread safety | pass | TSan 5/5 × 32 tests with `tests/tsan.supp` (OpenSSL/libdatachannel internals only, triaged) | fixed a use-after-free in the test receiver's teardown that TSan found |
| Clean shutdown with live browser peer | pass | 240 s streamer run with Chrome attached exited 0 | — |
| Core-only build without vcpkg | pass | `build` with `-DEME_WITH_WEBRTC=OFF -DEME_WITH_OPENH264=OFF`: 24/24 | — |
| SFU + P2P simultaneously | unresolved | EME-007 not built | — |
| Behaviour under real packet loss / slow link | unresolved | not tested (no netem run) | EME-011 |
| Bitrate at target | unresolved | the synthetic pattern encodes at ~37 kbps, which says nothing about 1.2 Mbps behaviour | needs real camera content (EME-004) |

## Review (Sonnet subagent, 2026-11-21)
| # | finding | outcome |
|---|---|---|
| 1 | stop() racing an onOpen past the token check could leave an unclosed peer whose callbacks captured `this` | fixed: `stopping_` checked under `m_` at insert |
| 2 | subscription notify captured `this`, which could be used after free if the server is destroyed before the producer | fixed: notify captures a `shared_ptr<WakeState>` |
| 3 | no handshake timeout (slowloris), maxPeers check-then-insert race, authenticated peer that never answers | fixed: `connectionTimeout`, atomic admit, 15 s answer deadline |
| 4 | last Peer reference could be released inside its own callback | fixed: dispatcher keeps closed peers for 5 s |
| 5 | `setBitrate` didn't raise the max bitrate; int overflow | fixed: sets MAX_BITRATE first, checks results, range-checks |
| 6 | forced keyframe lost if EncodeFrame fails | fixed: flag restored |
| 7 | PLI amplification relies on EncoderNode rate limit | accepted: default 500 ms; per-peer PLI limit listed in EME-008 follow-ups |
| T | tautological sharing assertion; no SPS profile check; missing malformed-input test | fixed (3 test changes) |

---

# Verification report: EME-014/015/016 (SFU publisher spikes) · 2026-11-21

Review method: live runs against local LiveKit 1.13.11 + Ingress 1.6.0 (Docker), unit tests and sanitizers, and an independent Sonnet reviewer of the WHIP code and the measurement method.

| requirement | verdict | evidence | correction |
|---|---|---|---|
| Separate ticket per spike + evaluation | pass | `tickets/EME-014`, `EME-015`, `EME-016` | — |
| SDK path publishes pre-encoded H.264, no re-encode | pass | eval: encoded/captured = 1.000; subscriber decodes 282 frames/track/20 s | first attempt delivered 0 frames: `VideoEncoderBackend::PreEncoded` was missing |
| WHIP path publishes via Ingress without transcoding | pass | Ingress log "Using proxied WHIP handler"; Ingress ~0.002 cores; 291 frames/track/20 s | — |
| Keyframe requests reach the shared encoder on both paths | pass | WHIP: 7 PLIs → 7 forced keyframes per camera; SDK: 10 feedback requests → 8 forced | — |
| Measurements comparable and repeatable | pass | 3 interleaved runs per mode on fresh stacks; ranges reported; stamp read on 100% of frames | review: unequal timestamps, mislabelled windows, process-start-based late-join metric and the size baseline were fixed, and the run repeated. The first two result sets were discarded |
| Encoder speed not a confounder | pass | 0 encoder-input drops at 15 fps in the final runs | Release spike build had linked vcpkg's debug OpenH264 (cached pkg-config); fixed with a fresh `build-spike` tree |
| WHIP security: token not leaked | pass | `WhipHttp.OriginComparisonGuardsTheToken`; DELETE only to same-origin resource | review finding A1 |
| Memory / thread safety after refactor + WHIP | pass | ASan+UBSan 2/2 × 37; TSan 5/5 × 37 with `tests/tsan.supp` | libstdc++ future suppression added (TSan cannot model the futex) |
| Report with recommendation | pass | `docs/reports/sfu-publisher-spike.md`, ADR-005 (proposed) | — |
| Results on Windows IoT target | unresolved | Linux i7-8550U only, all components on one host | rerun with EME-001 hardware |
| WHIP reconnect / bandwidth feedback | unresolved | not implemented in the spike | EME-007 follow-ups |

Remaining review items, not fixed and low risk: `resolveUrl` for query strings in relative Locations; `setupTimeout` applies to gathering and HTTP separately (up to 2×).

---

# Verification report: EME-017 (Linux camera source + multi-camera example) · 2026-11-21

Review method: real hardware (integrated Chicony camera + Logitech 046d:0821 USB camera), Chrome in the built-in browser,
ffmpeg as an independent reference, unit and integration tests, sanitizers, and an independent Sonnet reviewer (read-only).

| requirement | verdict | evidence | correction |
|---|---|---|---|
| Camera enumeration + capabilities report | pass | `camera_probe`: 2 cameras with full mode lists (the metadata node `/dev/video1` is correctly excluded) | review: odd `/dev/video*` names no longer abort enumeration |
| Capture via V4L2 → I420, using libraries for conversion | pass | libyuv `ConvertToI420` (MJPEG via libjpeg-turbo); `Camera.CapturesI420FramesWithMonotonicTimestamps` | — |
| Mode choice per PLAN §5.2 | pass | 720p15 → MJPEG on both cameras (raw YUYV at 720p tops out at 10 fps); integrated camera 30 → 15 by even dropping | review: zero-rate S_PARM, aspect tie-break, and the effective rate (not the requested one) passed downstream |
| Each camera encoded once, N viewers | pass | browser: 2 tabs → encoding 40 frames/s total (10+15+15) while 80 frames/s go out; 0 packets lost | — |
| Camera runs without viewers (PLAN §15) | pass | `/api/stats` with 0 viewers: captured ≈ encoded on both cameras | — |
| Output correctness | pass | Logitech mean luma 82 in our pipeline vs 87 from ffmpeg. The integrated camera is black in both (ffmpeg 8): covered by the privacy shutter, not a pipeline bug | — |
| Access control | pass | wrong token → HTTP 403; token constant-time compared, never logged; binds to 127.0.0.1 by default | — |
| CLI robustness | pass | `--size 1280` rejected; numbers must be digits and within range | review finding 7 |
| No orphan device on setup failure | pass | encoder created before the camera node is adopted | review finding 8 |
| Busy camera in tests | pass | EBUSY → `ResourceExhausted` → test skips | review finding 2 |
| Memory / thread safety | pass | ASan+UBSan 40/40 (+ 12 s `camera_viewer` run, both cameras); TSan 2/2 × 40 | — |
| Test fix found during verification | pass | the capture test held every frame, exhausting the 4-buffer pool (published 4, dropped 95) | test now consumes frames like a real consumer |
| Unplug → paused, other cameras continue; replug → resumes | pass | physical test: Logitech lost 23:29:28, back 23:29:43 via `/dev/video2` (was `/dev/video3`); viewer never reloaded, 15 fps, 0 loss, 0 freezes; `Camera.ResumesAfterDeviceLoss` (simulated loss) | added reconnect (2026-11-21). After review fixes: moved to another USB port (4.3 → 2), lost 23:34:33, back 23:34:46, viewer kept decoding (0 loss, 0 freezes) |
| Windows | pass (build) | EME-004 report below: CI 42/42 on windows-latest; hardware rows tracked there | — |

Reconnect review (Sonnet): wrong-camera matching (bus-only, name fallback stealing another source's camera) fixed by requiring name + bus
(bus may differ only if the camera is the sole one with that name), a process-wide claim registry, a sysfs-based candidate scan, and
initial-only format writes; `Camera.SecondSourceCannotClaimTheSameCamera` added; the reconnect test now asserts the paused state. 42/42 tests;
ASan clean; TSan 3/3 clean.

Remaining review items, not fixed: one camera failing at `STREAMON` stops the whole app (low severity); no pixel-content assertion in tests
(a covered camera is legitimately black).

---

# Verification report: EME-004 (Windows Media Foundation camera source) · 2026-11-21

Review method: Linux build and tests (no Windows machine or cross-compiler here), an independent Sonnet reviewer
(read-only, 12 findings), the owner's Windows machine (builds and runs, 2026-11-22), and Windows CI (`.github/workflows/windows.yml`, windows-latest, MSVC `/W4 /permissive-`, vcpkg manifest at the pinned baseline).

| requirement | verdict | evidence | correction |
|---|---|---|---|
| Same `camera.hpp` contract; `camera_viewer`/`camera_probe` unchanged | pass (source) | `src/sources/windows_mf/camera_mf.cpp`; no app/test source changes; mode choice, claims, effective fps, config validation shared with V4L2 via `src/sources/camera_common.hpp` | — |
| Compiles on Windows (MSVC) and all tests pass, camera tests skipped | pass | CI run [45120302046](https://github.com/SRai22/edge-media-engine/actions/runs/45120302046) (windows-latest, `x64-windows-static-md`): 42/42, the 4 hardware camera tests skipped (no camera), `Camera.OpenRejectsBadConfigAndMissingDevice` passed; 0 compiler warnings; owner: builds and runs on their Windows machine | first main run failed before compiling (GitHub 503 on a vcpkg spdlog patch download); `x64-windows` hit LNK2005 with libdatachannel's DLL, so the static triplet is used (#4); Release linked OpenH264's debug lib (LNK4098 `MSVCRTD` ×3), fixed by finding the release and debug libs separately — confirm on the next CI run |
| Linux still green after the shared-helper refactor | pass | `build/`: 42/42 with both real cameras attached (no skips), 0 compiler warnings | — |
| Enumeration: `MFEnumDeviceSources`, id = symbolic link, name = friendly name, bus = device instance | pass (source) | `listDevices()`, `instanceFromLink()` | — |
| Capabilities from the real device report | pass | device run 2026-11-23: `camera_probe` lists both cameras with YUY2/NV12/MJPG modes up to 1920×1080 at 30/15 fps | — |
| Mode choice: closest size, then NV12 > I420 > YUY2 > UYVY > MJPG reaching the rate | pass (source) | `kPreference` + shared `chooseMode` (same code path as Linux) | — |
| Async `IMFSourceReader`; callback never blocks | pass (review) | callback only converts into a pool buffer, publishes (non-blocking) and re-issues `ReadSample`; teardown gate is lock-free on the callback side (`busy_`/`owner_`) | review: gate first used `try_lock`, which would stall capture when `OnEvent` and `OnReadSample` overlapped; replaced before review |
| libyuv `ConvertToI420` + `FramePool` | pass (source) | contiguous buffer + `minBytes` length guard | — |
| Timestamps on `steady_clock`, monotonic across reconnects | pass (source) | MF time anchored per reader; falls back to arrival time if the driver's time does not advance | review #6 (frozen sample times would drop every frame) |
| Unplug → clean DeviceLost, no leaks; rediscover same camera by name + bus with claim registry | unresolved (hardware; the cameras are built in, so no physical unplug: runbook §3.4–3.7 now simulates removal with `Disable-PnpDevice`) | `MF_SOURCE_READERF_ERROR`/failed status/`MEVideoCaptureDeviceRemoved` → `deviceLost`; worker shuts the source down (RAII `wil::com_ptr`), reconnect mirrors Linux rules | owner: run runbook §3.4–3.7 (PnP disable/enable, 20-cycle leak check) |
| `simulateDeviceLossForTesting` | pass | device run 2026-11-23: `eme_tests.exe` 96/96, 0 skipped, incl. `Camera.ResumesAfterDeviceLoss`, `Camera.SecondSourceCannotClaimTheSameCamera`, `Camera.CapturesI420FramesWithMonotonicTimestamps` | — |
| 720p15 from each real camera (two cameras) | pass (capture) / unresolved (browser) | feasibility probe: both cameras NV12 1280×720 @15 together, 0 dropped over 60 s | `camera_viewer` in a browser on the device not run (runbook §3.3) |
| WIL for COM RAII; COM/MF init per thread | pass (source) | `wil::com_ptr_nothrow`, `wil::unique_cotaskmem_*`, WRL `RuntimeClass` for the callback; process-wide `CoIncrementMTAUsage` + `MFStartup` once, worker joins MTA via `wil::CoInitializeEx_failfast` | — |
| Runs with Media Foundation present but no camera | pass | CI: `Install-WindowsFeature Server-Media-Foundation` succeeded with no restart needed, `camera_probe.exe` printed `[]`, camera tests skipped | — |
| Runs where Media Foundation is absent (IoT package removed, N editions, Server) | pass (source) / unresolved | MF DLLs delay-loaded; `platform()` probes them, then `enumerateCameras()` is empty and `open()` returns DeviceLost. CI installs MF, so this path is not exercised there | owner: try on an image without the MF package if the IoT build removes it |

Review findings fixed: 1 (Strawberry Perl's pkg-config shadowing the Windows OpenH264 fallback), 2 (`wil` string out-params via `.put()`), 3 (explicit
`objbase.h`/WRL/`<atomic>`/`<functional>` includes), 4, 6, 8 (log before the worker starts), 10 (mid-stream type change → reopen with the original type).
Not changed: 9 (a loss injected while already lost fires after reconnect: same as Linux), 5/7/11 (notes, no defect), 12 (no stall watchdog: same as Linux).

---

# Verification report: EME-003 (JSON configuration loading) · 2026-11-22

Review method: unit tests (plain + ASan/UBSan), `synthetic_streamer` runs with and without `--config`, a build with
`-DEME_WITH_CONFIG=OFF`, and an independent Sonnet reviewer (read-only, 10 findings).

| requirement | verdict | evidence | correction |
|---|---|---|---|
| Uses nlohmann/json, no hand-written parser | pass | `src/config/engine_config.cpp` (`json::parse`, comments allowed); `find_package(nlohmann_json 3.11 …)` | review #6: version floor for `find(string_view)` |
| `loadConfig(path)` → `std::expected<EngineConfig, Error>` | pass | `include/eme/config/engine_config.hpp`; `Config.ShippedExampleLoads`, `Config.MissingFileIsAnError` | — |
| Every field validated at load, error names the field path | pass | 50 `Fields/ConfigRejects` cases: each field's type, range, even sizes, enum values, unknown keys, duplicate names/ids, required names | review #10: added bool/null/oversized-unsigned/non-object/upper-bound cases |
| Accepted values are accepted by the nodes | pass | `Config.AcceptedExtremesStart` (4×4@1 and 7680×4320@240 start) | review #1: 2×2 passed the loader but failed the source (min now 4) |
| Resource bounds | pass | frame pool capped at 512 MiB per source; bitrate 10 kbps–100 Mbps | review #2/#3: 7680×4320×64 pool (3.2 GB) or a 134 MB mock frame were accepted |
| Node names unambiguous | pass | `/` rejected in pipeline/sink names; sink `h264` rejected | review #4 |
| `synthetic_streamer --config file.json` builds its graph | pass | `configs/synthetic_streamer.json` run: 10 nodes, 6 sinks, 0 dependency violations, same output shape as without `--config`; bad file → exit 2 with field path | — |
| Existing behaviour and builds | pass | Linux 96/96 (42 previous + 54 config); `EME_WITH_CONFIG=OFF` builds, `--config` reports it is unavailable; 0 warnings | — |
| Windows (MSVC) | unresolved | not built here | runs in the Windows CI job on push |
| Resource-limit settings from config | unresolved | no resource manager exists yet | EME-009 adds them |

---

# Verification report: EME-001 (Milestone 0 feasibility probe) · 2026-11-23

Review method: written on Linux with no Windows compiler or hardware; independent Sonnet reviewer (read-only, 13 findings);
Windows CI builds the probe and smoke-runs it (no camera/GPU on the runner). Hardware results come from the device agent
following `docs/runbooks/eme-001-device.md`.

| requirement | verdict | evidence | correction |
|---|---|---|---|
| Probe app: cameras (all native types incl. H.264), encoder MFTs, two HW sessions, 2× capture + 2× encode, CPU/GPU/memory, JSON report | pass (source) | `apps/feasibility_probe/main.cpp`; report saved after every phase | — |
| Builds with MSVC and runs on the target | pass | built locally (VS 2022, static-md Release) and run on the Windows IoT i3-1115G4E; CI smoke run on push | first device run: HW sessions 0 frames out (`ProcessOutput 0x8000FFFF`): async MFT called twice per HaveOutput after a stream change; fixed `766e491` |
| Device runbook with exact commands and pass criteria | pass | `docs/runbooks/eme-001-device.md`, `tools/windows/eme001.ps1` | — |
| Camera capability report from the real device | pass | 2 cameras, 23 native types each, 1280×720 in NV12/MJPG/YUY2; no camera-native H.264 ([device report](docs/reports/eme-001-feasibility.md)) | — |
| Hardware encoder report incl. two concurrent sessions | pass | Intel Quick Sync H.264 MFT; 2/2 sessions, 75/75 frames each; CBR/1.2 Mbps/GOP 30/low latency/0 B-frames read back, baseline profile | — |
| Measured CPU/GPU for 2× capture + 2× encode (hw vs OpenH264) | pass | process CPU 1.9 % (hw) vs 5.1 % (OpenH264) of machine; 141 MB peak; ~1195 kbps per camera on hw; all 6 outputs decode in ffprobe at 1280×720 | PDH has no `VideoEncode` engine here; hw run shows 5 % `VideoDecode` |
| ADR: camera-native H.264 vs NV12 + HW encode | unresolved | evidence ready: no native H.264 on either camera, NV12 + Quick Sync works for 2 cameras | owner writes ADR-006 |

Review findings fixed: 1 (OpenH264 payload accessor: compile error), 2 (`strmiids` for CODECAPI GUIDs), 3 (MFStartup failure is
recorded, exit 0), 4 (camera graph destroyed before `MFShutdown`), 5 (measurement window ended at the deadline; kbps over the
measured time), 6 (output pulled again after a stream change), 7 (frame size checked before libyuv), 8 (drain parameter), 11, 13.
Not changed: 9 (first-output latency in camera runs includes the frame wait: documented in the runbook), 10 (`.put()` on WIL
strings already compiles in `camera_mf.cpp`), 12 (PDH per-process GPU instances: verify on the device, runbook notes the fallback).
Known limit: the probe links Media Foundation directly (no delay-load), so it needs MF installed.

---

# Verification report: EME-006 (Media Foundation hardware H.264 encoder) · 2026-11-23

Review method: written on Linux (no Windows compiler or Intel encoder); Linux build + tests for the shared parts; independent
Sonnet reviewer (read-only); Windows CI compiles it and runs the tests (encoder tests skip: no GPU on the runner). Hardware
results come from the device agent following `docs/runbooks/eme-006-device.md`.

| requirement | verdict | evidence | correction |
|---|---|---|---|
| `MfH264Encoder : IVideoEncoder` over the async hardware MFT | pass (source) | `src/processing/encoder/mf_h264_encoder.cpp`: events via `BeginGetEvent` queued to the encode thread; one `ProcessInput` per `METransformNeedInput`, one `ProcessOutput` per `METransformHaveOutput` (EME-001 device lesson) | — |
| `ICodecAPI`: low latency, CBR, bitrate, GOP, 0 B-frames, Baseline; read back and logged | pass | device 2026-11-23: read back profile 66, CBR, 1200000 bps, GOP 30 (test) / 60 (viewer), B-frames 0, low latency true; no warnings ([device report](docs/reports/eme-006-20261123.md)) | — |
| Forced keyframe via `CODECAPI_AVEncVideoForceKeyFrame`, ≤ 2 frames | pass | device: `MfH264Encoder.EncodesDecodableBaselineWithForcedKeyframe` OK. New-viewer keyframe in the browser not verified | — |
| Forced keyframes rate-limited (min interval, configurable) | pass | already in `EncoderNode` (`minKeyframeInterval`, default 500 ms) for every encoder | — |
| SPS/PPS into `CodecConfiguration`, `version` bumped on change | pass | Linux `H264AnnexB.*` 2/2; device: `EncodesDecodableBaselineWithForcedKeyframe` checks SPS/PPS in `codecConfig` | — |
| Output decodes (decoder + browser) | pass | device: every output decodes in OpenH264 with matching barcodes (test); Edge on the device plays the `--encoder mf` stream | — |
| Two sessions concurrently on the target | pass | device: `TwoSessionsConcurrently` OK; `camera_viewer --encoder mf --fps 30`: both cameras ~30.7 fps encoded, 0 dropped, `encoderInputDropped` 15/16 at start only; 9.8 % CPU vs 22.6 % OpenH264, 134 MB vs 65 MB | encoder read-back logged twice per camera at start (created twice?): not investigated |
| Open failure → `Unsupported`; software fallback | pass (source) | `create()` returns `Unsupported` without MF or a hardware MFT; `camera_viewer --encoder auto` (default) falls back to OpenH264 with a warning, `--encoder mf` fails instead | resource-policy-driven fallback waits for EME-009 |
| Linux still green | pass | `build/`: 98/98 (96 + 2 new `H264AnnexB` tests), 0 compiler warnings | — |
| Builds with MSVC `/W4`, Windows tests pass/skip | pass (local) | VS 2022 local build, 0 warnings; device 102/102, 4/4 encoder tests run (not skipped). Windows CI not checked | test did not compile: `Run` shadowed by `testing::Test::Run` → renamed `EncodeRun` (`e0e6787`) |
| Independent review | pass | Sonnet reviewer: 7 findings, all fixed (below) | — |
| Releases hardware sessions on destroy | pass | device: `ReleasesSessionsOnDestroy` 20 cycles OK | — |
| Viewer: keyframe on a new viewer, clean Ctrl+C exit | unresolved | not verified on the device (runbook §2.3, §2.5) | someone at the device |

Review findings fixed: 1 (`<strmif.h>` for `ICodecAPI`: compile error), 2 (a failed or stalled session now fails every later
`encode()` at once instead of blocking 2 s each time), 3 (wait for this frame's output, not for the next input request: one frame
of latency), 4 (event callback stops re-arming at teardown; `ReleasesSessionsOnDestroy` checks sessions are freed on the device),
5 (a forced keyframe whose `SetValue` fails is retried, and counted only after `ProcessInput` succeeds), 6 (requested profile,
bitrate, rate and size reapplied after a stream change, settings read back again), 7 (SPS/PPS fallback from
`MF_MT_MPEG_SEQUENCE_HEADER`). Not changed: test 8 (read-back assertions fail rather than skip if an MFT has no `GetValue`:
on the target that is a real finding).

Known limits: one system-memory buffer and an I420→NV12 copy per frame (`shortcut:` comment; EME-005 moves to D3D11 surfaces);
only hardware MFTs are tried (all are async; no synchronous path); a dead session makes `EncoderNode` log a warning per frame
(no automatic restart or fallback mid-stream yet: EME-010).
