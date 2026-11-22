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
- Build trees: `build/` (core only), `build-vcpkg/` (+ `-addr`, `-thre` sanitizer trees)
- Verification: `checks.md` · task notes: `sources/task.md`

## Completed
- **EME-002 / Milestone 1 (media core):** done on synthetic media. 23/23 tests pass on plain, ASan+UBSan and TSan builds
  (repeated 10×). The demo runs 2 sources → 2 encoders → 3 sinks each, with slow sinks dropping and recovering and 0 dependency violations.
- Independent review: 4 of 6 findings fixed, 2 ticketed (checks.md).
- **EME-008 direct WebRTC peers:** done for P2P. 32/32 tests (plain, ASan+UBSan, TSan). Chrome decodes both 720p15
  tracks with 0 loss. Sonnet review: 7 findings, 6 fixed, 1 accepted. OpenH264 software encoder added as fallback.

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

## Next action
EME-007: evaluate WHIP into LiveKit by reusing libdatachannel (ticket notes) against a local LiveKit server, then
verify SFU + P2P together. Alternatively EME-003 (JSON config, Haiku-sized) or EME-011 (netem loss test for P2P).
