# Progress

**Task:** Implement PLAN.md (edge media engine) incrementally with the loop workflow; long-term work as `tickets/EME-NNN-*.md`;
prefer established OSS libraries over custom code; subagents on Sonnet/Haiku.
**Constraint:** Development happens on Linux (no Windows device, Intel encoder or target cameras). Windows code is built in CI
(`.github/workflows/windows.yml`); anything needing the target hardware is run by an agent on the device from a runbook
(`docs/runbooks/`), and results come back as committed reports.

## Status by milestone (PLAN §20) · 2026-11-23

| Milestone | State |
|---|---|
| 0 Feasibility | **In progress (EME-001):** probe + runbook written, CI-built; waiting for the device run |
| 1 Media core | Done (EME-002), incl. JSON config (EME-003) |
| 2 Windows camera + HW encode | MF camera done (EME-004; hardware rows open). D3D11 buffers (EME-005) and MF H.264 encoder (EME-006) not started |
| 3 SFU publisher | Spikes done (EME-014/015/016); waiting for the owner's ADR-005 decision (WHIP recommended), then EME-007 |
| 4 Direct WebRTC | Done for P2P (EME-008) |
| 5 Performance/resilience | Camera reconnect done (Linux verified, Windows pending); rest not started (EME-009/010/011) |
| 6 OSS readiness | Not started (EME-013) |

## Outputs
- Core: `include/eme/{core,processing,sources,transport,config,testing}/`, `src/**`, `tests/**`, `CMakeLists.txt`, `vcpkg.json`
- Apps: `synthetic_streamer` (+ `--config`, `configs/synthetic_streamer.json`), `webrtc_streamer`, `camera_probe`, `camera_viewer`,
  `feasibility_probe` (Windows only, EME-001), `apps/spikes/**` (off by default)
- Camera backends: `src/sources/v4l2/` (Linux), `src/sources/windows_mf/` (Windows), shared `src/sources/camera_common.hpp`
- Device work: `docs/runbooks/eme-001-device.md`, `tools/windows/eme001.ps1`
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

## Decisions
- Custom bounded keyframe-aware queue (ADR-002); static typed DAG (ADR-003); libdatachannel + OpenH264, no GPL x264 (ADR-004).
- Keyframe requests rate-limited per encoder (default 500 ms).
- Media Foundation DLLs delay-loaded: MF is a removable package on Windows IoT.
- Windows builds use the static-md triplet (libdatachannel DLL import lib clashed at link time, LNK2005).
- Hardware-dependent work is handed to a device agent via runbooks; numbers come only from real runs.

## Open issues
- Owner decision pending: ADR-005 (WHIP vs SDK) blocks EME-007.
- EME-004 hardware rows (two cameras at 720p15, unplug/replug on Windows) unresolved until the device run (runbook §3).
- Confirm the LNK4098 (`MSVCRTD`) warning is gone in Windows CI after the OpenH264 lib fix.
- Not tested: WebRTC under real loss, SFU + P2P together, bitrate on real content, 24 h stress.
- Installed outside the repo (Linux dev box): `~/vcpkg`, `~/.local/bin/nasm`, `~/deps/livekit-sdk`, `~/deps/lk`, LiveKit Docker images.

## Next action
Device agent runs `docs/runbooks/eme-001-device.md` on the Windows i3 and commits the report (branch `device/eme-001-results`).
From its numbers: write ADR-006 (camera-native H.264 vs NV12 + HW encode) with the owner, then start EME-006 (MF hardware
H.264 encoder as `IVideoEncoder`, from the probe's `MftEncoder` prototype) followed by EME-005 (D3D11 zero-copy).
In parallel once the owner accepts ADR-005: EME-007 (production WHIP publisher).
