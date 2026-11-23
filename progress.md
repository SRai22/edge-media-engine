# Progress

**Task:** Implement PLAN.md (edge media engine) incrementally with the loop workflow; long-term work as `tickets/EME-NNN-*.md`;
prefer established OSS libraries over custom code; subagents on Sonnet/Haiku.
**Constraint:** Development happens on Linux (no Windows device, Intel encoder or target cameras). Windows code is built in CI
(`.github/workflows/windows.yml`); anything needing the target hardware is run by an agent on the device from a runbook
(`docs/runbooks/`), and results come back as committed reports.

## Status by milestone (PLAN §20) · 2026-11-23

| Milestone | State |
|---|---|
| 0 Feasibility | **Exit criterion met (EME-001, 2026-11-23):** 2 cameras → 2 Quick Sync sessions, decodable; ADR-006 pending |
| 1 Media core | Done (EME-002), incl. JSON config (EME-003) |
| 2 Windows camera + HW encode | MF camera done (EME-004; hardware rows open). MF H.264 encoder (EME-006) passes on the device (2026-11-23; new-viewer keyframe check open); D3D11 buffers (EME-005) not started |
| 3 SFU publisher | Spikes done (EME-014/015/016); waiting for the owner's ADR-005 decision (WHIP recommended), then EME-007 |
| 4 Direct WebRTC | Done for P2P (EME-008) |
| 5 Performance/resilience | Camera reconnect done (Linux verified, Windows pending); rest not started (EME-009/010/011) |
| 6 OSS readiness | Not started (EME-013) |

## Outputs
- Core: `include/eme/{core,processing,sources,transport,config,testing}/`, `src/**`, `tests/**`, `CMakeLists.txt`, `vcpkg.json`
- Apps: `synthetic_streamer` (+ `--config`, `configs/synthetic_streamer.json`), `webrtc_streamer`, `camera_probe`, `camera_viewer`,
  `feasibility_probe` (Windows only, EME-001), `apps/spikes/**` (off by default)
- Camera backends: `src/sources/v4l2/` (Linux), `src/sources/windows_mf/` (Windows), shared `src/sources/camera_common.hpp`
- Device work: `docs/runbooks/eme-001-device.md`, `tools/windows/eme001.ps1`, `docs/runbooks/eme-006-device.md`
- Reports: `docs/reports/sfu-publisher-spike.md` (+ `data/`); EME-001 device report goes to `docs/reports/eme-001-*.{json,md}`
- Not in git (owner convention): `tickets/`, `PLAN.md`, `docs/design-decisions/` (ADR-001…005)
- Build trees (Linux): `build-vcpkg/` (+ `-addr`, `-thre` sanitizer trees), `build-spike/`; worktrees use `-DVCPKG_INSTALLED_DIR=<repo>/build-vcpkg/vcpkg_installed`
- Verification: `checks.md` · task notes: `sources/task.md`

## Completed
- **EME-002 / Milestone 1:** media core on synthetic media; plain, ASan+UBSan and TSan.
- **EME-008:** direct WebRTC peers (libdatachannel) + OpenH264; Chrome decodes both 720p15 tracks with 0 loss.
- **EME-014/015/016:** SFU publisher spikes; WHIP recommended (ADR-005, proposed).
- **EME-017:** Linux V4L2 camera + `camera_viewer`; unplug/replug recovery verified physically.
- **EME-004:** Media Foundation camera source; Windows CI 42/42 (camera tests skipped), owner's Windows build runs.
  Static vcpkg triplet (`x64-windows-static-md`) + app-local VC++ runtime (#4).
- **EME-003:** JSON config with field-path errors; 96/96 Linux, ASan clean.

- **EME-001 device run (2026-11-23):** no camera-native H.264; NV12 + Quick Sync: 2/2 sessions, ~1195 kbps, 1.9 % CPU vs 5.1 %
  for OpenH264; all outputs decode. Probe async-MFT fix `766e491`. Report: `docs/reports/eme-001-feasibility.md`.

## Decisions
- Custom bounded keyframe-aware queue (ADR-002); static typed DAG (ADR-003); libdatachannel + OpenH264, no GPL x264 (ADR-004).
- Keyframe requests rate-limited per encoder (default 500 ms).
- Media Foundation DLLs delay-loaded: MF is a removable package on Windows IoT.
- Windows builds use the static-md triplet (libdatachannel DLL import lib clashed at link time, LNK2005).
- Hardware-dependent work is handed to a device agent via runbooks; numbers come only from real runs.

## Open issues
- Owner decision pending: ADR-005 (WHIP vs SDK) blocks EME-007.
- EME-004 hardware rows still open: both viewer tiles not visually confirmed; removal/return not run. Cameras are built in, so runbook §3.4–3.7 simulates removal with PnP disable/enable.
- Device has no build tools: build on a Windows PC, copy `build-win-static\Release\*.exe,*.dll` to the device and run there.
- Not tested: WebRTC under real loss, SFU + P2P together, bitrate on real content, 24 h stress.
- Installed outside the repo (Linux dev box): `~/vcpkg`, `~/.local/bin/nasm`, `~/deps/livekit-sdk`, `~/deps/lk`, LiveKit Docker images.

## Next action
EME-006: device run done (`docs/reports/eme-006-20261123.md`); open: keyframe on a new viewer, and a one-viewer memory number
(134 MB was with two viewers). Owner: write ADR-006 from
`docs/reports/eme-001-feasibility.md` (evidence points to NV12 + hardware encode). Then EME-005 (D3D11 zero-copy into the MF
encoder). Device agent also runs eme-001-device.md §3.4–3.7 (PnP disable/enable + leak cycles, elevated, owner OK first).
In parallel once the owner accepts ADR-005: EME-007 (production WHIP publisher).
