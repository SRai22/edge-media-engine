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
