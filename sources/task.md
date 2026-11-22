# Task notes (temporary)

- Request (2026-11-21): implement PLAN.md using the loop workflow; long-term work as `tickets/EME-NNN-*.md`; subagents on Sonnet/Haiku.
- User preference: use established OSS libraries instead of custom code (memory: prefer-oss-libraries).
- Environment checked 2026-11-21: Ubuntu 24.04, g++ 13.3, CMake 3.28, GoogleTest 1.14, spdlog 1.12. No Ninja.
  **No Windows device, cameras, or Intel encoder available**, so M0/M2+ hardware work is blocked (PLAN §24.2: synthetic only).
- Sources used: PLAN.md only. No external API docs were fetched this session (no dependency integrations attempted).

## EME-008 session (2026-11-21)
- libdatachannel v0.24.6 headers read from GitHub (`include/rtc/*.hpp`, `src/rtppacketizer.cpp`) at tag v0.24.6, 2026-11-21.
  Latest tag v0.24.7 (published 2026-11-21) was not used: it is newer than the vcpkg baseline and less than two weeks old.
- vcpkg 2026.10.16 (commit 9eec002d…) cloned to ~/vcpkg. Port versions: libdatachannel 0.24.6, openh264 2.6.0,
  nlohmann-json 3.12.0, spdlog 1.17.0, gtest 1.18.0.
- nasm 2.16.01 copied from Ubuntu's own package (`apt-get download`) into ~/.local/bin (no sudo available).
- The system FFmpeg has only libx264 (GPL) for software H.264, so it is not used.

## EME-014/015/016 session (2026-11-21)
- LiveKit C++ SDK v1.13.1 prebuilt `livekit-sdk-ubuntu-24.04-x64-1.13.1.tar.gz`, sha256 matched the GitHub asset digest.
  `include/livekit/*.h` read for API (EncodedVideoSource, TrackPublishOptions::video_encoder, VideoStream).
- LiveKit server v1.13.11 and Ingress v1.6.0 Docker images; lk CLI v2.19.2 (digest verified). Ingress config fields from
  `livekit/ingress` v1.6.0 `pkg/config/config.go`; `enable_transcoding` semantics from `livekit/protocol` `livekit_ingress.proto`.
- libcurl 8.23.0 via vcpkg (license curl/ISC/BSD-3).
