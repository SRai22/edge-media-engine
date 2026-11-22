# SFU publisher spike: WHIP vs LiveKit C++ SDK

**Tickets:** EME-014 (SDK), EME-015 (WHIP), EME-016 (evaluation) · **Decides:** EME-007 · **Date:** 2026-11-21
**Raw data:** [data/sfu-publisher-eval-20261121.json](data/sfu-publisher-eval-20261121.json) · **Harness:** `spikes/sfu_eval/run_eval.py`, `summarize.py`

## Recommendation (proposed, owner decides; see ADR-005)

**Publish to the SFU over WHIP on libdatachannel.**
- **Runtime:** the two paths are equal on CPU, encode count and latency.
- **Footprint:** WHIP is clearly smaller. The publisher uses −25 MB RSS and 6 fewer threads. The binary is +0.7 MB over the P2P build we ship anyway, against +32.8 MB for the SDK libraries.
- **Recovery:** WHIP recovers a late or lossy viewer about 2.5× faster (room connected → first frame: 85 ms vs 216–238 ms).
- **One WebRTC stack:** we already ship libdatachannel for direct peers (PLAN §6.2).

The costs are on the server and in features:
- You must run LiveKit Ingress. It only proxies signaling: about 11 MB and ~0.002 cores in our runs.
- WHIP lacks what the SDK gives for free: reconnection, data tracks, and bandwidth-estimate feedback. These are follow-up work (below).

## What was compared
| | WHIP (EME-015) | SDK (EME-014) |
|---|---|---|
| Stack on device | libdatachannel 0.24.6 (already used for P2P) + libcurl | LiveKit C++ SDK 1.13.1 prebuilt (Rust FFI + Google libwebrtc) |
| Server | LiveKit 1.13.11 + Ingress 1.6.0, `enable_transcoding=false` | LiveKit 1.13.11 |
| Media path | publisher → LiveKit SFU (Ingress logs "Using proxied WHIP handler": signaling only, no media hop) | publisher → LiveKit SFU |
| Pre-encoded input | RTP packetizer on our access units | `EncodedVideoSource` + `VideoEncoderBackend::PreEncoded` |
| Keyframe requests | RTCP PLI → `PliHandler` → shared encoder | `takeFeedback()` polled every 20 ms → shared encoder |

Identical workload for both: 2 × 1280×720 @ 15 fps, moving random-block texture (realistic encoder load), OpenH264 at 1.2 Mbps,
GOP 600 frames (40 s), so recovery inside a run depends on keyframe requests. A 64-bit capture timestamp is drawn into the luma
plane; `Barcode.SurvivesOpenH264At720p` proves it survives a real 1.2 Mbps encode and decode.

**Run timeline** (fresh LiveKit stack per run, 3 runs per mode, interleaved):
- t=0: publisher and subscriber A start.
- t=25 s: subscriber B joins late.
- t=45 s: `lk load-test` adds 10 subscribers for 15 s.
- t=70 s: stop.

**Measurement:**
- Subscribers use the same LiveKit SDK client for both modes. They decode to I420 and read the stamp; latency is measured on the shared CLOCK_MONOTONIC.
- CPU and memory are sampled at 1 Hz from `/proc` (publisher) and cgroup v2 (containers).
- `SDK-PTS` is the SDK fed the same regular media timeline WHIP uses for RTP. It was added after review found that the first run fed the two paths different timestamps.

## Results (median [min–max] of 3 runs)

Host: Intel(R) Core(TM) i7-8550U CPU @ 1.80GHz · 2026-11-21 22:17 · stripped size: WHIP 10.9 MB, SDK 32.8 MB (incl. liblivekit + liblivekit_ffi), EME-008 P2P baseline 10.2 MB

| Metric | WHIP | SDK | SDK-PTS |
|---|---|---|---|
| Runs | 3 | 3 | 3 |
| Publisher setup (ms) | 112 [111–261] | 141 [128–155] | 133 [131–201] |
| Cold start → first decoded frame (ms) | 711 [277–909] | 1339 [1296–1343] | 1321 [1318–1408] |
| Late join → first decoded frame (ms, from process start) | 239 [186–397] | 357 [308–427] | 325 [312–388] |
| Late join: room connected → first frame (ms) | 85 [81–115] | 238 [176–276] | 216 [193–218] |
| Capture→decode latency p50 (ms) | 94 [74–128] | 105 [84–111] | 75 [60–91] |
| Capture→decode latency p95 (ms) | 126 [91–138] | 141 [130–150] | 104 [94–133] |
| Late joiner latency p95 (ms) | 114 [111–126] | 122 [121–160] | 97 [78–129] |
| Frames decoded, sub A (per track, 20 s) | 291 [288–299] | 282 [282–283] | 282 [282–284] |
| Unreadable stamps (all subs) | 0 [0–0] | 0 [0–0] | 0 [0–0] |
| Publisher CPU, 1 viewer (cores) | 0.89 [0.88–1.44] | 0.86 [0.84–0.88] | 0.87 [0.84–0.88] |
| Publisher CPU, 10 load-test viewers (cores) | 0.91 [0.89–0.93] | 0.92 [0.91–0.94] | 0.98 [0.92–1.02] |
| Publisher RSS (MB) | 50.2 [50.1–52.8] | 74.6 [74.6–74.7] | 74.7 [74.6–75.1] |
| Publisher threads | 18 [18–18] | 24 [24–24] | 24 [24–24] |
| LiveKit CPU, 1 viewer (cores) | 0.023 [0.023–0.055] | 0.023 [0.023–0.023] | 0.023 [0.022–0.025] |
| LiveKit CPU, 10 load-test viewers (cores) | 0.081 [0.078–0.089] | 0.075 [0.070–0.077] | 0.089 [0.072–0.093] |
| Server total CPU, 10 viewers (LiveKit + Ingress, cores) | 0.084 [0.079–0.090] | 0.077 [0.072–0.078] | 0.091 [0.076–0.095] |
| Server total memory, 10 viewers (MB) | 76 [76–76] | 57 [57–59] | 58 [58–60] |
| Ingress CPU (cores) / memory (MB) | 0.002 [0.001–0.003] / 11 [11–11] | — (not used) | — (not used) |
| Encoded / captured (both cams) | 1.000 [1.000–1.000] | 1.000 [1.000–1.000] | 1.000 [1.000–1.000] |
| Keyframe requests reaching encoder (per cam) | 7 [7–7] | 10 [10–10] | 10 [10–10] |
| Forced keyframes (per cam, 70 s) | 7 [7–7] | 8 [8–8] | 8 [8–8] |

## Update: Ingress is not needed (2026-11-21, smoke test)
livekit-server v1.13.11 serves WHIP natively at `/whip/v1` (`pkg/service/whipservice.go`), authenticated by a normal
LiveKit join token as the Bearer token. The unchanged `WhipPublisher` published there directly: 2 tracks decoded, PLI forwarded
(2 PLIs → 2 forced keyframes), late joiner's first frame 83 ms after connect. This was one run, not re-measured with the full harness.
For WHIP, Ingress therefore adds only a stream-key indirection. The SFU can be a single `livekit-server` process (native
Windows/Linux/macOS binaries exist), with no Redis, no Ingress and no Docker required.

## Reading the numbers
- **Encode once holds on both paths.** Encoded/captured = 1.000 with 1, 2 and 10 viewers. Viewers never add encoder work.
- **CPU is a tie** (~0.9 cores), dominated by the two OpenH264 encoders. Server CPU is a tie as well.
- **Latency is a tie.** p50 is 75–105 ms across modes, with overlapping ranges. The first (discarded) run showed a WHIP advantage that disappeared once the SDK got the same timestamps (`SDK-PTS`). Timestamp handling matters more than the transport.
- **Late-join recovery favours WHIP**, consistently: 85 [81–115] ms against 216–238 ms, with no overlap. The 20 ms feedback poll explains at most ~20 ms of that. The rest is the SDK's FFI and feedback path (not further attributed). The SDK also issues more keyframe requests (10 vs 7 per camera) and forces one more keyframe per camera.
- **Footprint favours WHIP:**
  - Publisher RSS 50 MB vs 75 MB, and 18 vs 24 threads.
  - Stripped binary +0.7 MB over the EME-008 P2P baseline (10.2 → 10.9 MB), against +32.8 MB of SDK libraries. A product with P2P and SFU would ship about 11 MB with WHIP versus about 43 MB with the SDK, which carries two WebRTC engines.
- **Server memory favours the SDK** (+19 MB on the server for WHIP, mainly the Ingress service). That cost lands on the server, not the edge device.

## Caveats
- Measured on a Linux x86 laptop (i7-8550U), not the Windows IoT i3 target. Everything ran on one host (publisher, decoders, load test, SFU). Treat absolute numbers as relative; rerun on target hardware (EME-001).
- The SDK path uses the vendor's prebuilt Ubuntu binaries. A Windows x64 prebuilt exists (`livekit-sdk-windows-x64-1.13.1.zip`) but was not measured.
- Three runs per mode; ranges are shown so overlaps are visible.
- WHIP Ingress logs the stream key in plain text (LiveKit's logging). Treat Ingress logs as secret, or rotate keys.

## If WHIP is chosen: follow-up work (EME-007)
1. Reconnect / ICE restart for the WHIP session (EME-010).
2. Bandwidth feedback: the SDK reported a target bitrate. WHIP has to use RTCP REMB/TWCC from the SFU; check what libdatachannel exposes.
3. Two sessions (one per camera) vs one session with two tracks: today it's one session. Ingress names both tracks "synthesized-camera", so track identity needs a convention.
4. Short-lived stream keys and HTTPS to Ingress outside localhost (PLAN §17).
