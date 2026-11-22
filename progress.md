# Progress

**Task:** Implement PLAN.md (edge media engine) incrementally with the loop workflow; long-term work as `tickets/EME-NNN-*.md`;
prefer established OSS libraries over custom code; subagents on Sonnet/Haiku.
**Constraint:** This environment is Linux with no Windows device, cameras or Intel encoder. Hardware milestones are blocked (PLAN §24.2).

## Outputs
- Code: `CMakeLists.txt`, `vcpkg.json`, `include/eme/**`, `src/**`, `tests/**`, `apps/synthetic_streamer/main.cpp`
- Docs: `README.md`, `docs/architecture.md`, `docs/design-decisions/ADR-00{1,2,3}-*.md`
- Tickets: `tickets/EME-001` … `EME-013`
- Verification: `checks.md` · task notes: `sources/task.md`

## Completed
- **EME-002 / Milestone 1 (media core):** done on synthetic media. 23/23 tests pass on plain, ASan+UBSan and TSan builds
  (repeated 10×). The demo runs 2 sources → 2 encoders → 3 sinks each, with slow sinks dropping and recovering and 0 dependency violations.
- Independent review: 4 of 6 findings fixed, 2 ticketed (checks.md).

## Decisions
- Custom bounded keyframe-aware queue: no library offers flush-and-wait-for-keyframe semantics (ADR-002).
- Keyframe requests are rate-limited per encoder (default 500 ms) after the demo showed one slow viewer inflating keyframes 10 → 7 in 3 s.
- Static typed DAG with `Producer<T>`/`Consumer<T>` ports instead of a plugin/caps system (ADR-003).
- Test doubles live in the `eme::mock` namespace (`eme::testing` collides with GoogleTest).

## Open issues
- Windows/MSVC build and vcpkg manifest unverified (EME-012).
- JSON config not implemented. Use nlohmann/json (EME-003).
- Hardware work (EME-001, 004–006) needs the target device. WebRTC (EME-007/008) can start on Linux against the mock encoder.

## Next action
EME-008: add libdatachannel via vcpkg/apt, verify its `H264RtpPacketizer`/`PliHandler` API against the pinned version,
and build a `Consumer<EncodedVideoFrame>` peer node fed by the mock encoder, checked by a browser on localhost.
(Alternatively EME-003 first: small and Haiku-sized.)
