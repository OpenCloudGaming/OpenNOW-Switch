# OpenNOW Switch

Native Horizon OS homebrew prototype for running the OpenNOW GeForce NOW client flow on a modded Nintendo Switch.

This is not an Electron port. Electron/Chromium/Node do not run as a Switch applet, and the Switch web applets are not a usable GeForce NOW browser runtime. This project is a native C++20 `.nro` scaffold that ports the pieces OpenNOW can realistically reuse: GFN session orchestration boundaries, signaling boundaries, stream settings, diagnostics, and the GFN input packet encoder.

## Current Status

Implemented:

- CMake project with host/mock build and Switch packaging hooks.
- devkitPro Switch environment validation and post-package `.nro`/`.nacp` validation.
- Native runtime split inspired by Moonlight-Switch, without copying GPL source.
- GFN service boundary for auth/session/media work.
- Auth URL/token request builders, PKCE generation, OAuth callback listener, token exchange/refresh service, and mockable HTTP transport.
- Optional libcurl-backed live HTTPS client for host builds.
- CloudMatch session-create request builder for the v1 profile.
- CloudMatch create/poll/stop/claim request builders.
- CloudMatch session response parsing into native stream/signaling/media endpoint state.
- WSS signaling URL/protocol helper.
- SDP/nvstSdp helpers required for the signaling offer/answer exchange.
- C++ port of OpenNOW's GFN input encoder for heartbeat, keyboard, mouse, gamepad, and haptics packets.
- Controller mapper for Joy-Con/Pro Controller state into GFN/XInput-style packets.
- Small Switch-safe JSON parser for protocol responses.
- Host-side tests for the binary input protocol.

Not implemented yet:

- NVIDIA OAuth callback UI on Switch.
- Switch-side mbedTLS/libcurl runtime validation.
- WSS signaling socket.
- Native WebRTC/ICE/SCTP/RTP media transport.
- FFmpeg NVTEGRA decoder and deko3d renderer integration.
- Audren audio output.

The hard blocker is native GFN media transport, not UI or packaging.

## Build: Host Mock

```powershell
cmake -S . -B build/host -G Ninja
cmake --build build/host
ctest --test-dir build/host --output-on-failure
```

Run:

```powershell
.\build\host\opennow-switch.exe
```

Test the host NVIDIA login flow:

```powershell
.\build\host\opennow-switch.exe --login
.\build\host\opennow-switch.exe --refresh
```

`--login` prints an NVIDIA login URL, waits for the localhost OAuth redirect, exchanges the code, fetches user info, fetches a client token, and stores the auth session in `%APPDATA%\OpenNOW-Switch\auth-session.json`. `--refresh` reloads that session and refreshes the access token.

## Build: Switch

Install devkitPro/devkitA64/libnx first. Then configure with the Switch toolchain available in your devkitPro install:

```sh
cmake -S . -B build/switch -G Ninja \
  -DPLATFORM_SWITCH=ON \
  -DOPENNOW_BUILD_TESTS=OFF \
  -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake"
cmake --build build/switch --target OpenNOW.nro
```

On this Windows install, devkitPro is at `C:\devkitPro` and its toolchain requires the bundled MSYS2 CMake. This command path is known to work here:

```powershell
C:\devkitPro\msys2\usr\bin\bash.exe -lc "export DEVKITPRO=/opt/devkitpro DEVKITA64=/opt/devkitpro/devkitA64; cd /c/Users/Zortos/Projects/OpenNOW-Switch; cmake -S . -B build/switch-msys -G 'Unix Makefiles' -DPLATFORM_SWITCH=ON -DOPENNOW_BUILD_TESTS=OFF -DOPENNOW_ENABLE_CURL=OFF -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/Switch.cmake"
C:\devkitPro\msys2\usr\bin\bash.exe -lc "export DEVKITPRO=/opt/devkitpro DEVKITA64=/opt/devkitpro/devkitA64; cd /c/Users/Zortos/Projects/OpenNOW-Switch; cmake --build build/switch-msys --target OpenNOW.nro -j2"
```

The Switch configure now fails fast by default if `DEVKITPRO`, `DEVKITA64`, `nacptool`, or `elf2nro` are missing. The `OpenNOW.nro` target validates that:

- `OpenNOW.nro` exists and contains the `NRO0` header magic.
- `OpenNOW.nacp` exists and is at least the expected NACP size.
- the NACP metadata contains the configured title and author.

You can check the local devkitPro environment without doing a Switch build:

```powershell
cmake --build build/host --target validate-switch-env
```

On a machine without devkitPro configured, that target is expected to fail and list the missing variables/tools.

Launch through title redirection/full RAM. Album applet mode is unsupported for streaming.

The current NRO shows a console status screen and stays open until `PLUS (+)` is pressed.
The status screen is now a fixed-width dashboard with account state, off-device login state, game categories, runtime readiness, and recent categorized logs. It uses category logs such as `[AUTH]`, `[SESSION]`, `[SIGNALING]`, `[MEDIA]`, and `[INPUT]` so it is clear which parts are implemented and which are still blocked.

The Steam Deck-style login path found in the researched JavaScript is represented natively:

- request device authorization with `POST /device/authorize`
- display `verification_uri`, `verification_uri_complete`, and `user_code`
- poll `POST /token` with `grant_type=urn:ietf:params:oauth:grant-type:device_code`

The current NRO has the dashboard and protocol support. Live Switch QR rendering/polling still needs Switch TLS dependencies before it can contact NVIDIA directly.

## Architecture

- `app`: runtime coordinator.
- `platform`: mock and Switch platform hooks.
- `gfn`: GeForce NOW protocol boundaries.
- `core`: settings and shared state.

The Moonlight-Switch reference is used for architecture only: CMake/libnx packaging shape, full-RAM requirement, FFmpeg/deko3d/Audren direction, and controller-oriented runtime split. No Moonlight-Switch GPL code is copied into this tree.
