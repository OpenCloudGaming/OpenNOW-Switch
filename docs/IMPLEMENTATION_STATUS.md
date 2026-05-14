# Implementation Status

This project is a native Switch scaffold for OpenNOW. It intentionally does not claim playable GeForce NOW streaming yet.

## Completed

- Host/mock CMake build.
- Switch `.nro` packaging hooks for devkitPro/libnx.
- Switch console lifecycle and `appletMainLoop()` hold state so the NRO stays open until PLUS is pressed.
- Categorized runtime logging for APP, PLATFORM, AUTH, NETWORK, SESSION, SIGNALING, MEDIA, INPUT, and BUILD status.
- Fixed-width Switch dashboard for account status, off-device login state, game categories, runtime readiness, and recent logs.
- devkitPro environment validator for `DEVKITPRO`, `DEVKITA64`, `nacptool`, and `elf2nro`.
- Post-package `.nro`/`.nacp` validator for artifact existence, NRO magic, NACP size, and title/author metadata.
- Host-side self-test for the NRO validator.
- Verified Switch package build with `C:\devkitPro\msys2\usr\bin\cmake.exe`; generated and validated `build/switch-msys/OpenNOW.nro`.
- Title-mode/app-mode guard point.
- Stream settings model for v1: 720p60, H264, stereo audio.
- GFN service boundary with OpenNOW desktop client constants.
- Auth URL/token request builders.
- PKCE verifier/challenge generation for NVIDIA OAuth.
- Steam Deck-style device authorization builders and parser for `/device/authorize` and device-code `/token` exchange.
- OAuth localhost callback listener.
- Token exchange, user-info fetch, client-token fetch, and refresh-token service.
- Host `--login` and `--refresh` commands with auth session persistence.
- Mockable HTTP transport boundary.
- Optional libcurl-backed live HTTPS transport for host builds.
- CloudMatch session-create request builder for the v1 profile.
- CloudMatch create/poll/stop/claim request builders.
- CloudMatch session response parser for signaling URL, server IP, media endpoint, queue position, GPU type, and ICE servers.
- GFN WSS signaling URL/protocol helper.
- SDP/nvstSdp helpers for offer/answer processing.
- C++ GFN input encoder with host tests.
- Joy-Con/Pro Controller to GFN gamepad mapper with host tests.
- Small Switch-safe JSON parser with host tests.
- Native media pipeline interfaces for transport, decoder, renderer, and audio.

## Next Required Work

1. Validate live HTTPS transport against devkitPro/libnx with mbedTLS/libcurl on Switch.
2. Add NVIDIA OAuth UI on Switch, likely URL/QR entry plus callback or manual-code fallback.
3. Implement native WSS signaling socket.
4. Parse offer messages, build answers, and send SDP/nvstSdp through WSS.
5. Evaluate `libdatachannel` for Switch. If it is too large or not portable enough, implement the minimal GFN-compatible ICE/RTP/SCTP/datachannel path.
6. Integrate FFmpeg NVTEGRA H264 decoding.
7. Integrate deko3d YUV rendering.
8. Integrate Audren stereo audio playback.
9. Wire Switch HID polling to `ControllerMapper` and `InputEncoder`.

## Licensing Boundary

Moonlight-Switch was used as an architecture reference only. No Moonlight-Switch GPL source was copied into this project.
