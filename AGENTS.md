# AGENTS.md

## Core Priorities

1. Keep streaming latency low and frame delivery bounded.
2. Preserve reliability across reconnects, partial streams and decoder fallback.
3. Keep controller, touch and keyboard input predictable.

Choose correctness and recovery over a local shortcut. Performance changes must
retain bounded queues and must not weaken the existing resynchronization paths.

## Repository Layout

- `app/src/` contains the launcher, typed GeForce NOW API surface and shared
  application state.
- `app/src/gfn/` contains the GeForce NOW implementation, split by
  authentication, catalog, cloud sessions, regions and persistence.
- `app/src/webrtc/` contains signaling negotiation, media, input, diagnostics
  and peer-session lifecycle behind `webrtc_session.hpp`.
- `app/src/stream/` owns audio, video decoding and rendering. Deko3D is the
  Switch path; OpenGL is the host/reference path.
- `app/src/StreamView.cpp`, `stream_view_input.cpp` and
  `stream_view_overlay.cpp` own stream lifecycle/input dispatch, keyboard and
  NTE automation, and stream overlays respectively.
- `app/src/settings_tab*.cpp` splits settings layout/state, page construction,
  account actions and stream/preference actions.
- `resources/` contains RomFS fonts, translations, icons and UI assets.
- `tests/` contains small host-side policy and parsing tests.
- `scripts/build-switch-msys2.sh`, `build-switch.ps1` and
  `scripts/package-release.ps1` are the supported build and packaging entry
  points. `scripts/build-forwarder-installer.ps1` rebuilds the bundled HOME
  forwarder installer.
- User-facing documentation lives at `https://opennow.zortos.me`; keep the
  repository README focused on source, build and attribution information.
- `extern/` is vendored third-party code. Do not edit it unless a task
  explicitly requires an upstream dependency patch.

## Module Boundaries

- Keep NVIDIA/GFN HTTP, authentication, account persistence and catalog parsing
  behind `gfn_client.hpp`; implementations belong in the focused files under
  `app/src/gfn/`. UI views should consume typed models instead of rebuilding
  requests.
- Keep CloudMatch orchestration in `gfn/cloud_session.cpp`, request and response
  protocol handling in `gfn/cloud_session_protocol.cpp`, and trace/persisted
  active-session state in `gfn/cloud_session_state.cpp`. Shared declarations
  stay private in `gfn/cloud_session_internal.hpp`.
- Keep signaling and peer-connection state under `app/src/webrtc/`. WebSocket
  framing belongs in `WebSocketClient.*` and the `SignalingClient` wrapper.
- Keep stream-setting defaults and persistence in `stream_settings.*`; put
  independently testable selection rules in focused `*_policy.hpp` headers.
- Keep cover download, inspection and deletion in `cover_image_cache.*`;
  settings UI code must not traverse or mutate cache directories directly.
- Keep settings page construction separate from immediate account and
  stream/preference actions. Do not grow `settings_tab.cpp` back into a single
  all-purpose implementation.
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
- Cache paths must derive from `AppHomePath()` rather than repeating the current
  `sdmc:/switch/SwitchNOW` location.
- Keep account, settings and credential writes atomic and preserve backup
  recovery behavior.
- Never log bearer tokens, passwords, session cookies or decrypted credentials.
- TLS verification, authentication headers and persisted-data compatibility are
  security boundaries, not refactoring conveniences.

## Maintainability

- Prefer a small typed helper beside its owning subsystem over a broad utility
  module.
- Split large translation units by ownership and lifecycle, not arbitrary line
  count. Keep internal cross-file contracts in narrowly scoped `*_internal.hpp`
  headers and retain local-only helpers in anonymous namespaces.
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
- `tests/ice_candidate_pair_policy_test.cpp` and `tests/rtcp_nack_test.c` use
  headers under `extern/libpeer/src`; the RTCP test also links
  `extern/libpeer/src/rtcp.c`.
- For Switch-facing changes, run `bash scripts/build-switch-msys2.sh` in a
  devkitPro MSYS2 environment, or `./build-switch.ps1` from PowerShell. Override
  parallelism with `OPENNOW_BUILD_JOBS` when necessary.
- When a full dependency build is unavailable, directly compile every affected
  devkitA64 object and, for a file split, perform a relocatable link of the
  related objects to catch duplicate definitions. This is a partial check and
  must not be reported as a successful NRO build.
- Do not claim the Switch build passed from a host-only compile. If the devkitPro
  toolchain is unavailable, report that limitation and still run all applicable
  host tests.
- The host RTCP NACK test currently has an endian-sensitive assertion failure on
  arm64 macOS. Report it separately; do not hide it or attribute it to unrelated
  changes.

## Development Environment

- The production target is Nintendo Switch homebrew using devkitA64, libnx,
  Deko3D, Borealis and the pinned libraries under `extern/`.
- The unified `SwitchNOW.nro` build contains NVDEC and software-decoder fallback;
  there is no separate NVDEC build flavor or legacy NVDEC build script.
- CMake requires `DEVKITPRO` and configures `Switch.cmake` before the project.
  Standard desktop CMake configuration is therefore not a valid build check.
- A fresh libpeer/Mbed TLS bootstrap may run Python generators that require the
  `jsonschema` module. If it is missing, report the dependency-bootstrap
  limitation rather than claiming the application failed to compile.
- The output and persisted-data names remain `SwitchNOW.nro` and
  `sdmc:/switch/SwitchNOW/` for compatibility. Renaming them requires an
  explicit migration covering accounts, settings, shortcuts and recovery data.
- Full login and gameplay tests require a real NVIDIA/GeForce NOW account and
  Switch hardware. Never add credentials or captured sessions to tests.
- For nxlink deployment from this macOS workstation, T3 Code child processes
  are blocked by Local Network privacy. Run nxlink through Terminal with
  `sudo /opt/devkitpro/tools/bin/nxlink -a 192.168.1.101 build/switch/SwitchNOW.nro`.
  Confirm the IP displayed by the Switch NetLoader before sending if the
  network has changed.
