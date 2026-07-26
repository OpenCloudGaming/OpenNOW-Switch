# OpenNOW for Nintendo Switch

OpenNOW is a native GeForce NOW client for Nintendo Switch homebrew. This
repository contains the Switch launcher, streaming runtime, HOME-screen
shortcut installer, host-side policy tests, and pinned build dependencies.

OpenNOW is under active development and is not affiliated with, endorsed by, or
sponsored by NVIDIA or Nintendo. A valid GeForce NOW account is required.

## Documentation

Product documentation, setup guidance, and troubleshooting are maintained at
[opennow.zortos.me](https://opennow.zortos.me).

This Switch port currently retains the inherited `SwitchNOW.nro` filename and
`sdmc:/switch/SwitchNOW/` data directory. Changing either requires a deliberate
migration so existing accounts, settings, shortcuts, and recovery data remain
compatible.

## Build

The production target is Nintendo Switch homebrew and requires devkitPro,
devkitA64, Switch portlibs, CMake, Ninja, and the pinned dependencies under
`extern/`.

From PowerShell with MSYS2 and devkitPro installed:

```powershell
.\build-switch.ps1
```

From a configured devkitPro MSYS2 shell:

```bash
bash scripts/build-switch-msys2.sh
```

The unified output is:

```text
build/switch/SwitchNOW.nro
```

It includes NVDEC hardware decoding and the software-decoder fallback; there is
no separate NVDEC build flavor.

Create a release archive with:

```powershell
.\scripts\package-release.ps1 -Version 0.0.5
```

## Repository layout

```text
app/src/                 Launcher, GFN client, streaming, and UI
app/shortcut/            Per-game shortcut chainloader
resources/               RomFS fonts, translations, icons, and UI assets
extern/                  Pinned third-party dependencies
tests/                   Host-side policy and parsing tests
scripts/                 Supported build and packaging entry points
tools/opennow-forwarder-installer/
                         HOME-screen forwarder installer
```

## Development checks

Compile a header-only host test with:

```bash
g++ -std=c++20 -Wall -Wextra -Werror -Iapp/src tests/<name>.cpp
```

Tests that exercise implementation files must compile those `.cpp` files and
link their host dependencies. Host checks do not replace a devkitA64 Switch
build.

## Acknowledgements

This port is derived from
[SwitchNOW](https://github.com/Blade-Punisher/SwitchNOW) by
[Blade-Punisher](https://github.com/Blade-Punisher).

Core dependencies include
[Borealis](https://github.com/xfangfang/borealis),
[libpeer](https://github.com/sepfy/libpeer),
[FFmpeg](https://github.com/FFmpeg/FFmpeg),
[libnx](https://github.com/switchbrew/libnx),
[deko3d](https://github.com/devkitPro/deko3d),
[curl](https://github.com/curl/curl),
[Jansson](https://github.com/akheron/jansson), and
[zlib](https://github.com/madler/zlib).

Third-party license and attribution files remain alongside their vendored
components.
