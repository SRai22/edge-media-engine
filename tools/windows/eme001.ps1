# EME-001 device run: build, test, probe cameras + hardware encoders, validate the H.264 output.
# Run from the repo root in a "Developer PowerShell for VS 2022" on the target device, with both cameras plugged in:
#
#   powershell -ExecutionPolicy Bypass -File tools\windows\eme001.ps1 [-Seconds 60] [-SkipBuild] [-Vcpkg $HOME\vcpkg]
#
# Everything lands in eme001-out\ (gitignored); copy only report.json into docs/reports/.
param(
  [int]$Seconds = 60,
  [switch]$SkipBuild,
  [string]$Vcpkg = "$HOME\vcpkg",
  [string]$Build = "build-win",
  [string]$Out = "eme001-out"
)
$ErrorActionPreference = "Stop"
New-Item -ItemType Directory -Force $Out | Out-Null
$log = Join-Path $Out "run.log"
function Step($name) { Write-Host "==> $name" -ForegroundColor Cyan; Add-Content $log "==> $name $(Get-Date -Format o)" }

Step "environment"
"git: $(git rev-parse --short HEAD)" | Tee-Object -Append $log
Get-CimInstance Win32_OperatingSystem | Select-Object Caption, Version, BuildNumber | Format-List | Out-String | Tee-Object -Append $log
Get-CimInstance Win32_Processor | Select-Object Name, NumberOfCores, NumberOfLogicalProcessors | Format-List | Out-String | Tee-Object -Append $log
Get-CimInstance Win32_VideoController | Select-Object Name, DriverVersion, DriverDate | Format-List | Out-String | Tee-Object -Append $log
Get-PnpDevice -Class Camera, Image -PresentOnly -ErrorAction SilentlyContinue | Select-Object FriendlyName, Status, InstanceId |
  Format-Table -AutoSize | Out-String -Width 300 | Tee-Object -Append $log

if (-not $SkipBuild) {
  Step "configure + build (Release, x64-windows-static-md)"
  cmake -S . -B $Build -A x64 "-DCMAKE_TOOLCHAIN_FILE=$Vcpkg\scripts\buildsystems\vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows-static-md 2>&1 | Tee-Object -Append $log
  cmake --build $Build --config Release --parallel 2>&1 | Tee-Object -Append $log
  if ($LASTEXITCODE -ne 0) { throw "build failed (see $log)" }
}
$bin = Join-Path $Build "Release"

Step "unit + camera tests (cameras attached: camera tests must RUN, not skip)"
ctest --test-dir $Build -C Release --output-on-failure 2>&1 | Tee-Object (Join-Path $Out "ctest.txt")

Step "camera_probe"
& "$bin\camera_probe.exe" | Tee-Object (Join-Path $Out "camera_probe.json")

Step "feasibility_probe ($Seconds s per encoder run)"
& "$bin\feasibility_probe.exe" --seconds $Seconds --out $Out 2>&1 | Tee-Object -Append $log | Out-Null
if (-not (Test-Path (Join-Path $Out "report.json"))) { throw "feasibility_probe wrote no report" }

Step "validate H.264 output with ffprobe (if installed)"
$ffprobe = Get-Command ffprobe -ErrorAction SilentlyContinue
if ($ffprobe) {
  Get-ChildItem $Out -Filter *.h264 | ForEach-Object {
    $info = & ffprobe -v error -f h264 -count_frames -select_streams v:0 `
      -show_entries stream=profile,width,height,nb_read_frames -of json $_.FullName 2>&1
    "$($_.Name): $info" | Tee-Object -Append (Join-Path $Out "ffprobe.txt")
  }
} else {
  "ffprobe not installed: H.264 files not validated (ask the owner before installing ffmpeg)" | Tee-Object -Append $log
}

Step "summary"
$r = Get-Content (Join-Path $Out "report.json") -Raw | ConvertFrom-Json
"cameras: " + (($r.cameras | ForEach-Object { "$($_.name) nativeH264=$($_.nativeH264)" }) -join "; ")
"hardware encoders: " + (($r.encoders.hardware | ForEach-Object { $_.name }) -join ", ")
"concurrent hardware sessions working: $($r.concurrentSessions.concurrentSessionsWorking) of $($r.concurrentSessions.sessionsRequested)"
foreach ($run in $r.captureEncode) {
  "[$($run.encoder)] process CPU $([math]::Round($run.resources.processCpuPercentOfMachine,1))% of machine, " +
  "system CPU $([math]::Round($run.resources.systemCpuPercent,1))%, peak WS $([math]::Round($run.resources.peakWorkingSetMB))MB"
  foreach ($c in $run.perCamera) {
    "   $($c.camera): captured $($c.capture.framesCaptured), encoded $($c.encode.framesOut), keyframes $($c.encode.keyframes), " +
    "$([math]::Round($c.encode.kbps)) kbps $(if ($c.error) { 'ERROR ' + $c.error }) $(if ($c.encode.error) { 'ERROR ' + $c.encode.error })"
  }
}
Write-Host "Done. Results in $Out (commit only report.json, see docs/runbooks/eme-001-device.md)." -ForegroundColor Green
