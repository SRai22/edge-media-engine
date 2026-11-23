# EME-001 feasibility: device results · 2026-11-23

Device: Windows 10 IoT Enterprise LTSC 2021 (21H2, build 19044), Intel Core i3-1115G4E (11th gen, 2C/4T), Intel UHD Graphics
(0x9A78). Cameras: `TDOC_STB_FIXED` (USB 2574:0560) and `TDOC_STB_PTZ` (USB 2574:055F).
Binaries built on a Windows dev PC (`x64-windows-static-md`, Release, commit `766e491`) and copied to the device, which has no
build tools. Run: `feasibility_probe.exe --seconds 60`. Full report: [`eme-001-20261123.json`](eme-001-20261123.json).

The first device run (commit `816f98a`) got 0 frames out of every hardware session: `ProcessOutput 0x8000FFFF` after one frame in.
Cause: on `MF_E_TRANSFORM_STREAM_CHANGE` the probe called `ProcessOutput` again, but an async MFT allows one call per
`METransformHaveOutput`. Fixed in `766e491`; the numbers below are from the rerun.

## Milestone 0 checks (runbook §2)

| check | result | verdict |
|---|---|---|
| Both cameras enumerated with native types | 2 cameras, 23 native types each; 1280×720 in NV12, MJPG and YUY2 at 30/15 fps | pass |
| Camera-native H.264 | **not offered by either camera** (`nativeH264: false`) | input to the ADR |
| Hardware H.264 encoder | "Intel Quick Sync Video H.264 Encoder MFT" (VEN_8086, listed twice), plus the software "H264 Encoder MFT" | pass |
| Two concurrent hardware sessions | 2/2; 75/75 frames each, 3 keyframes each, first output 143 ms / 111 ms | pass |
| Encoder honours settings | read back: CBR (0), 1 200 000 bps, GOP 30, low latency on, 0 B-frames; profile 66 (baseline). All `SetValue` S_OK | pass |
| Both cameras capture at once | negotiated NV12 1280×720 @15 on both; captured 902 / 901, published 894 / 893, dropped 0, no device loss | pass |
| 2× capture + 2× HW encode | encoded 893 / 892 (= frames in), 30 keyframes each, 1196 / 1194 kbps | pass |
| Output decodes (ffprobe 9.0.2) | all 6 files Constrained Baseline 1280×720; frames read = frames encoded (see below) | pass |
| Resource use | table below | measured |

ffprobe (`-count_frames`, profile, width, height, nb_read_frames):

```
capture_hw_0.h264:       Constrained Baseline,1280,720,893
capture_hw_1.h264:       Constrained Baseline,1280,720,892
capture_openh264_0.h264: Constrained Baseline,1280,720,900
capture_openh264_1.h264: Constrained Baseline,1280,720,901
session_0.h264:          Constrained Baseline,1280,720,75
session_1.h264:          Constrained Baseline,1280,720,75
```

Resources, 2 cameras at 720p15, 60 s each:

| encoder | process CPU (% of machine) | system CPU | peak working set | GPU (this process, avg utilisation) | kbps per camera |
|---|---|---|---|---|---|
| Quick Sync (hw) | 1.9 % | 28.2 % | 141 MB | VideoDecode 5.0 %, 3D 1.5 % | 1196 / 1194 |
| OpenH264 | 5.1 % | 23.6 % | 141 MB | — (no GPU engine use) | 376 / 1133 |

Notes:
- PDH reports no `VideoEncode` engine on this driver; the hardware encoder's work appears under `VideoDecode` (0 % in the
  OpenH264 run). Likely Intel's shared media engine under another name; not confirmed.
- System CPU is the whole device (other services were running); compare process CPU between the two runs.
- OpenH264 on `TDOC_STB_FIXED` stays at 376 kbps against a 1200 kbps target on a mostly static scene; Quick Sync's CBR holds the target.
- The encoder's friendly name is double-UTF-8-encoded in the report ("IntelÃ‚Â®"); cosmetic probe bug.
- `firstOutputLatencyMs` in the camera runs (≈330 ms hw) includes waiting for the first camera frame (runbook §2 caveat).

**Milestone 0 exit criterion met:** both cameras capture simultaneously and produce decodable H.264 through NV12 + Quick Sync.
With no camera-native H.264, the ADR-006 choice is NV12 + hardware encode by default.

## 30 fps run

Same build and device, `feasibility_probe.exe --seconds 60 --fps 30`. Full report: [`eme-001-20261123-30fps.json`](eme-001-20261123-30fps.json).

| | 15 fps | 30 fps |
|---|---|---|
| Two concurrent HW sessions | 2/2, 75/75 frames | 2/2, 150/150 frames |
| Negotiated | NV12 1280×720 @15 | NV12 1280×720 @30 (both cameras) |
| HW: captured / dropped | 902, 901 / 0 | 1828, 1827 / 0 |
| HW: encoded, kbps | 893, 892 · 1196, 1194 | 1788, 1790 · 1196, 1198 |
| HW: process CPU (% of machine) | 1.9 % | 4.5 % |
| HW: GPU (this process) | VideoDecode 5.0 %, 3D 1.5 % | VideoDecode 10.1 %, 3D 2.9 % |
| OpenH264: encoded, kbps | 900, 899 · 376, 1133 | 1800, 1799 · 483, 1190 |
| OpenH264: process CPU (% of machine) | 5.1 % | 11.3 % |
| System CPU (hw / OpenH264 run) | 28.2 % / 23.6 % | 29.5 % / 38.4 % |
| Peak working set | 141 MB | 144 MB |

ffprobe at 30 fps: `capture_hw_0/1` 1788 / 1790 frames, `capture_openh264_0/1` 1800 / 1799, `session_0/1` 150 / 150; all
Constrained Baseline 1280×720.

`camera_viewer --fps 30` (OpenH264) with Edge on the device, one viewer, `/api/stats` sampled 10 s apart: each camera
captured and encoded ~31 frames/s, 0 dropped, no device loss; 63 frames/s sent to the viewer (2 tracks); the page shows
"live" with video. Viewer process CPU 22.6 % of machine, working set 65 MB. OpenH264 warned of ~21 fps input during the
first seconds, then kept pace.

## EME-004 hardware checks (runbook §3)

| check | result |
|---|---|
| 1. Capabilities from the real device | pass: `camera_probe` lists both cameras with modes (YUY2 up to 1920×1080, NV12/MJPG 640×360–1920×1080, 30/15 fps) |
| 2. Camera tests ran on hardware | pass: `eme_tests.exe` 96/96, 0 skipped; `Camera.CapturesI420FramesWithMonotonicTimestamps`, `Camera.SecondSourceCannotClaimTheSameCamera`, `Camera.ResumesAfterDeviceLoss` passed |
| 3. Two cameras in the browser | run at 30 fps (above): Edge on the device plays, ~31 fps encoded per camera, 0 dropped. Both tiles not visually confirmed (screenshot shows the first tile only) |
| 4–6. Removal and return | not run. The cameras are built into the device, so they can't be unplugged; runbook §3.4–3.6 now uses `Disable-PnpDevice` / `Enable-PnpDevice`. Other-port test not applicable |
| 7. No leaks across removal | not run (runbook §3.7: 20 disable/enable cycles) |
