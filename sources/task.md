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
