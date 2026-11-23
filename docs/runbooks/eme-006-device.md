# Runbook: EME-006 hardware H.264 encoder on the Windows target

For the agent working **on the target device** (Windows 10 IoT, i3-1115G4E, Intel Quick Sync, cameras `TDOC_STB_FIXED`
and `TDOC_STB_PTZ`). Same ground rules as [eme-001-device.md](eme-001-device.md): work on `device/eme-006-results`, ask the
owner before pushing or installing anything, record failures as they happen, and never invent numbers.

The device has no build tools. Build Release (`x64-windows-static-md`) on a Windows PC from this branch and copy
`build-win\Release\*.exe` and `*.dll` to the device, as for EME-001.

## 1. Encoder tests (must pass, not skip)

```powershell
.\eme_tests.exe --gtest_filter=MfH264Encoder.* 2>&1 | Tee-Object eme006-tests.txt
```

| test | checks |
|---|---|
| `RejectsInvalidConfigAndInput` | bad configs are rejected; only I420/NV12 at the configured size are accepted |
| `EncodesDecodableBaselineWithForcedKeyframe` | I420 and NV12 input, 60 frames at 720p15: settings read back (profile 66, CBR, 1.2 Mbps, GOP 30, 0 B-frames), SPS/PPS in `codecConfig`, keyframe within 2 frames of a request, every output decodes in OpenH264 and its barcode matches its own input frame |
| `TwoSessionsConcurrently` | two sessions on two threads, both produce output and keyframes |
| `ReleasesSessionsOnDestroy` | 20 create/encode/destroy cycles all succeed (hardware sessions are freed) |

**Pass:** all four `[       OK ]`. `[  SKIPPED ]` means no hardware MFT was found, which is a failure on this device:
record the skip message. Then run the whole suite (`.\eme_tests.exe`) and record the totals.

## 2. Two cameras through Quick Sync in the browser

```powershell
.\camera_viewer.exe --encoder mf --fps 30 2>&1 | Tee-Object eme006-viewer.log
```

`--encoder mf` fails instead of falling back to OpenH264, so a hardware problem shows up. At start the log prints one line per
camera like `Intel® Quick Sync Video H.264 Encoder MFT: profile 66, CBR, 1200000 bps, GOP 60, B-frames 0, low latency true,
forced keyframes supported`. Record those lines and any `did not accept every requested setting` warning.

Open the printed URL in Edge on the device, then:

1. **Both tiles play.** Take a screenshot that shows both tiles.
2. **Rate:** sample `/api/stats` twice, 10 s apart (as in eme-001-device.md §3.3). Record `encoded`, `captured`,
   `dropped` and `encoderInputDropped` per camera. Pass: about 30 encoded frames per second per camera, and
   `encoderInputDropped` staying near 0 (the EME-001 probe lost about 2 % of frames at 30 fps in its 2-frame queue).
3. **Keyframe on request:** reload the page. The new viewer sends a PLI, so `keyframes` goes up and video appears within about a second.
4. **CPU and memory:** record camera_viewer's CPU and working set in Task Manager after a minute. Compare them with the
   OpenH264 viewer run from EME-001 (22.6 % of the machine, 65 MB at 30 fps).
5. Stop with Ctrl+C. It must exit cleanly (no hang, no crash dialog).

## 3. What to commit (on `device/eme-006-results`)

- `docs/reports/eme-006-<yyyymmdd>.md`: test results (counts and any failure text), the read-back lines, the §2 numbers, CPU/memory
  next to the OpenH264 run, and the screenshot.
- `checks.md`: set the EME-006 device rows (requirement | verdict | evidence | correction).
- Not logs, binaries or build trees.

If something crashes or hangs, keep `eme006-viewer.log` and record the last lines.
