# AGENTS.md

## Core Priorities

1. Keep streaming latency low and frame delivery bounded.
2. Preserve reliability across reconnects, partial streams and decoder fallback.
3. Keep controller, touch and keyboard input predictable.

Choose correctness and recovery over a local shortcut. Performance changes must
retain bounded queues and must not weaken the existing resynchronization paths.

## Repository Layout

- `app/src/` contains the launcher, GeForce NOW API client and streaming runtime.
- `app/src/stream/` owns audio, video decoding and rendering. Deko3D is the
  Switch path; OpenGL is the host/reference path.
- `resources/` contains RomFS fonts, translations, icons and UI assets.
- `tests/` contains small host-side policy and parsing tests.
- `scripts/` and `build-switch.ps1` contain the supported build and packaging
  entry points.
- `extern/` is vendored third-party code. Do not edit it unless a task
  explicitly requires an upstream dependency patch.

## Module Boundaries

- Keep NVIDIA/GFN HTTP, authentication, account persistence and catalog parsing
  in `gfn_client.*`; UI views should consume its typed models instead of
  rebuilding requests.
- Keep signaling and peer-connection state in `webrtc_session.*`. WebSocket
  framing belongs in `WebSocketClient.*` and the `SignalingClient` wrapper.
- Keep stream-setting defaults and persistence in `stream_settings.*`; put
  independently testable selection rules in focused `*_policy.hpp` headers.
- Keep platform-specific rendering behind `IVideoRenderer`. Do not introduce
  Deko3D details into UI code or assume OpenGL exists on Switch.
- Preserve the public data-channel report formats and the Xbox-compatible input
  contract when changing controller code.

## Streaming and Concurrency

- The network worker owns `peer_connection_loop`; the decoder worker owns decode
  submission; the UI thread polls signaling and renders completed frames.
- Keep `decoder_queue_` bounded. On congestion, dropping stale work and
  requesting a keyframe is preferable to growing latency.
- Do not hold `peer_mutex_` while performing unrelated file or UI work. Maintain
  the established lock ordering when touching peer, decoder queue or logging
  state.
- Avoid busy polling. If a worker has no work, use the existing backoff policy
  or a condition variable while keeping wake-up latency explicit.
- Diagnostic logging is opt-in. Do not add per-packet or per-frame logging to
  the normal gameplay path.

## Persistence and Security

- Runtime data belongs under the paths provided by `app_paths.*`; do not add
  repository-local account, token or log files.
- Keep account, settings and credential writes atomic and preserve backup
  recovery behavior.
- Never log bearer tokens, passwords, session cookies or decrypted credentials.
- TLS verification, authentication headers and persisted-data compatibility are
  security boundaries, not refactoring conveniences.

## Maintainability

- Prefer a small typed helper beside its owning subsystem over a broad utility
  module.
- Reuse existing policy headers for behavior that can be tested without Switch
  hardware.
- Match the existing C++20 style and keep public interfaces stable unless the
  task requires a contract change.
- Do not mix inherited SwitchNOW-to-OpenNOW renaming with unrelated behavior
  changes; paths, package names and persisted data require an explicit migration.

## Checks

- Run the narrowest relevant host test first. A header-only test can be compiled
  with `g++ -std=c++20 -Wall -Wextra -Werror -Iapp/src tests/<name>.cpp`.
- When a test exercises a `.cpp` implementation, compile that implementation
  into the same host test and link its host dependency, such as Jansson.
- For Switch-facing changes, run `./scripts/build-switch-msys2.sh` in a devkitPro
  MSYS2 environment, or `./build-switch.ps1` from PowerShell.
- Do not claim the Switch build passed from a host-only compile. If the devkitPro
  toolchain is unavailable, report that limitation and still run all applicable
  host tests.

## Development Environment

- The production target is Nintendo Switch homebrew using devkitA64, libnx,
  Deko3D, Borealis and the pinned libraries under `extern/`.
- The unified `SwitchNOW.nro` build contains NVDEC and software-decoder fallback;
  do not use the legacy NVDEC script as a separate build flavor.
- CMake requires `DEVKITPRO` and configures `Switch.cmake` before the project.
  Standard desktop CMake configuration is therefore not a valid build check.
- Full login and gameplay tests require a real NVIDIA/GeForce NOW account and
  Switch hardware. Never add credentials or captured sessions to tests.
