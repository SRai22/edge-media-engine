# Runbook: EME-001 feasibility + EME-004 camera checks on the Windows target

For the agent (or person) working **on the target device**: Windows IoT, Intel Core i3 (device run: 11th gen i3-1115G4E), two USB cameras.
The code was written and CI-built on Linux with no access to this hardware. Your job is to run it here and record
what actually happens. Never invent or estimate numbers: if something fails, record the failure.

Ground rules:
- Work on a branch (`git switch -c device/eme-001-results`), commit only what section 5 lists, and ask the owner before pushing.
- Do not install software (ffmpeg, drivers) without asking the owner first.
- `tickets/`, `PLAN.md` and `docs/design-decisions/` stay out of git (owner convention).

## 0. Prerequisites (check once)

```powershell
git --version; cmake --version
& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property displayName   # VS 2022 + Desktop C++
Get-PnpDevice -Class Camera,Image -PresentOnly | Select FriendlyName,Status        # both cameras, Status OK
Get-CimInstance Win32_VideoController | Select Name,DriverVersion                   # Intel UHD/Iris; note the driver version
```

Camera access for desktop apps must be on: *Settings → Privacy & security → Camera → Let desktop apps access your camera*.
If vcpkg is missing, follow README "Build: Windows (MSVC + vcpkg)". vcpkg must be checked out at the `builtin-baseline` in `vcpkg.json`.

## 1. Build, test and probe (one command)

In a **Developer PowerShell for VS 2022**, from the repo root, with both cameras plugged in and no other app using them:

```powershell
powershell -ExecutionPolicy Bypass -File tools\windows\eme001.ps1 -Seconds 60
```

It builds Release, runs `ctest`, `camera_probe`, then `feasibility_probe` (about 2.5 min: synthetic encode test,
60 s cameras → hardware encoders, 60 s cameras → OpenH264), validates the `.h264` files with ffprobe if present,
and prints a summary. Outputs: `eme001-out\report.json`, `ctest.txt`, `camera_probe.json`, `run.log`, `*.h264`.

Rerun just the probe: `build-win\Release\feasibility_probe.exe --seconds 60 --out eme001-out`.
If a camera has no 1280×720 mode, the report says so; rerun with `--size WxH` using a size from `nativeTypes`.

## 2. Pass criteria: EME-001 (Milestone 0)

Read `eme001-out\report.json`:

| check | where in report.json | pass when |
|---|---|---|
| Both cameras enumerated with native types | `cameras[*].nativeTypes` | 2 entries, each lists 1280×720 (any format) |
| Camera-native H.264 available? | `cameras[*].nativeH264` | record true/false (input to the ADR, not pass/fail) |
| Hardware H.264 encoder present | `encoders.hardware` | ≥ 1 entry (expect "Intel® Quick Sync Video H.264 Encoder MFT") |
| Two concurrent hardware sessions | `concurrentSessions.concurrentSessionsWorking` | `2`; each `sessions[i].result.framesOut` ≈ `framesPerSession`, no `error` |
| Encoder honours settings | `concurrentSessions.sessions[0].codecApi`, `.profile` | record `readBack` values (CBR, bitrate, GOP, low latency, B-frames 0) and profile (66 = baseline) |
| Both cameras capture at once | `captureEncode[0].perCamera[*].capture` | `framesPublished` ≈ 15 × seconds each, `framesDropped` ≈ 0, `deviceLost` false |
| 2× capture + 2× HW encode works | `captureEncode[0].perCamera[*].encode` | `framesOut` ≈ `framesPublished`, `keyframes` ≥ seconds / 2, `kbps` near 1200, no `error` |
| Output decodes | `ffprobe.txt` (or play a `.h264` in VLC/ffplay) | every file: `nb_read_frames` > 0, 1280×720 |
| Resource use measured | `captureEncode[*].resources` | record `processCpuPercentOfMachine`, `systemCpuPercent`, `gpu.thisProcess.VideoEncode`, `peakWorkingSetMB` for **hw** and **openh264** |

Caveats: `firstOutputLatencyMs` in the camera runs includes waiting for the first camera frame (the probe's event loop
blocks on input), so it is not encoder latency; the synthetic `concurrentSessions` value is closer. Check that
`resources.gpu.thisProcess` is non-empty; if it is empty, record `allProcesses.VideoEncode` instead and say so.

Milestone 0 exit criterion: both cameras capture simultaneously **and** produce decodable H.264 through one validated path.

## 3. EME-004 hardware checks (Media Foundation camera source)

Use the same build. Each check maps to an unresolved row in `checks.md` (EME-004 section).

1. **Capabilities from the real device:** `camera_probe.json` lists both cameras with modes (format, size, fps).
2. **Camera tests ran on hardware:** `ctest.txt` shows `Camera.CapturesI420FramesWithMonotonicTimestamps`,
   `Camera.SecondSourceCannotClaimTheSameCamera` and `Camera.ResumesAfterDeviceLoss` **Passed** (not Skipped).
3. **Two cameras at 720p15 in the browser:**
   ```powershell
   build-win\Release\camera_viewer.exe
   ```
   Open the printed `http://127.0.0.1:8640/?token=…` URL in Edge or Chrome on the device. Both tiles play. Then:
   ```powershell
   $u = "<the printed URL, with /api/stats instead of />"   # keep ?token=…
   Invoke-RestMethod $u | ConvertTo-Json -Depth 5
   ```
   Record each camera's negotiated format (console line `<camera>: NV12 1280x720 @30 fps (emitting 15) …` at start) and
   encoded frames per second (`encoded` in `/api/stats`, sampled twice 10 s apart).
The cameras are wired inside the embedded device, so physical unplugging isn't possible. Steps 4–7 simulate the removal by
disabling the camera's PnP device instead. Windows tears the device down for applications just as it would on a removal, and
Media Foundation raises `MEVideoCaptureDeviceRemoved`. This changes device state, so ask the owner first. It needs an
**elevated** PowerShell. The instance id stays the same, so this tests rediscovery by name + bus, not a move to another port.

```powershell
$cam = Get-PnpDevice -Class Camera -PresentOnly | Where-Object FriendlyName -eq 'TDOC_STB_FIXED'   # camera A, exactly one row
$cam | Format-List FriendlyName, InstanceId, Status                                               # check it is the right one
$stats = "<the printed URL, with /api/stats instead of />"                                        # keep ?token=…
$vw = { (Get-Process camera_viewer).WorkingSet64 / 1MB }
```

4. **Removal during capture:** with the viewer open, run `Disable-PnpDevice -InstanceId $cam.InstanceId -Confirm:$false`.
   Expect: A's tile pauses, B keeps playing, the console logs `camera lost`, and `Invoke-RestMethod $stats` shows A with
   `"deviceLost": true`. No crash.
5. **Return:** `Enable-PnpDevice -InstanceId $cam.InstanceId -Confirm:$false`. Within ~2 s the console logs `camera back`,
   the tile resumes without reloading the page, and `reconnects` = 1.
6. **Other port:** not applicable (fixed wiring). Instead, repeat 4–5 on camera B (`TDOC_STB_PTZ`) and check that A keeps playing.
7. **No leaks:** with the stream running, record `& $vw`, then cycle camera A 20 times and record it again after a minute:
   ```powershell
   & $vw
   1..20 | ForEach-Object {
     Disable-PnpDevice -InstanceId $cam.InstanceId -Confirm:$false; Start-Sleep 3
     Enable-PnpDevice  -InstanceId $cam.InstanceId -Confirm:$false; Start-Sleep 5
   }
   Start-Sleep 60; & $vw; Invoke-RestMethod $stats | ConvertTo-Json -Depth 5
   ```
   Pass: `reconnects` for A went up by 20, both tiles play, and the working set is within a few MB of the first reading.
   If a cycle leaves A disabled (the script was stopped), run `Enable-PnpDevice` again before finishing.

If `Disable-PnpDevice` says a restart is required, or the device stays in `Status` OK, something vetoed the removal
while the camera was open. Don't reboot. Record that result: it means this method can't simulate removal on this device.

## 4. ADR input

From the report, decide (with the owner) between **camera-native H.264** and **NV12 + hardware encode** (PLAN §7).
Write the evidence into `docs/design-decisions/ADR-006-capture-encode-path.md` (stays out of git): native H.264
availability per camera, two-session result, CPU/GPU for hw vs OpenH264, and the profile the encoder accepted.

## 5. What to commit (on `device/eme-001-results`)

- `docs/reports/eme-001-<yyyymmdd>.json`: copy of `eme001-out\report.json` (contains device names and camera symbolic links, nothing secret).
- `docs/reports/eme-001-feasibility.md`: short summary table of section 2 with the real numbers, the ffprobe lines,
  and the EME-004 check results from section 3.
- `checks.md`: set the EME-004 hardware rows and add an EME-001 section (requirement | verdict | evidence | correction).
- Not the `.h264` files, logs or build trees.

If anything in the probe crashes or hangs, keep `eme001-out\run.log` and the partial `report.json`
(it is saved after every phase) and report the last `[eme001]` phase line printed.
