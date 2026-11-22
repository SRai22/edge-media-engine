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
