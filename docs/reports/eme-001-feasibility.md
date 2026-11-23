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

## EME-004 hardware checks (runbook §3)

| check | result |
|---|---|
| 1. Capabilities from the real device | pass: `camera_probe` lists both cameras with modes (YUY2 up to 1920×1080, NV12/MJPG 640×360–1920×1080, 30/15 fps) |
| 2. Camera tests ran on hardware | pass: `eme_tests.exe` 96/96, 0 skipped; `Camera.CapturesI420FramesWithMonotonicTimestamps`, `Camera.SecondSourceCannotClaimTheSameCamera`, `Camera.ResumesAfterDeviceLoss` passed |
| 3. Two cameras at 720p15 in the browser | not run (needs a browser on the device); the probe shows both cameras capturing NV12 720p15 together |
| 4–6. Unplug / replug same and other port | not run (needs someone at the device) |
| 7. No leaks across replug | not run |
