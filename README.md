<h1 align="center">OpenNOW for Nintendo Switch</h1>

<p align="center">
  <img src="resources/img/opennow-logo-mark.png" alt="OpenNOW cloud logo" width="180" />
</p>

<p align="center">
  <strong>A native, controller-first GeForce NOW client for Nintendo Switch homebrew.</strong>
</p>

<p align="center">
  Browse your library, launch a cloud session, and play through an open-source client built for the Switch.
</p>

<p align="center">
  <a href="https://github.com/OpenCloudGaming/OpenNOW-Switch/releases/latest">
    <img src="https://img.shields.io/github/v/release/OpenCloudGaming/OpenNOW-Switch?style=for-the-badge&label=Download&color=E60012&logo=nintendoswitch&logoColor=white" alt="Download OpenNOW for Nintendo Switch">
  </a>
  <a href="https://opennow.zortos.me">
    <img src="https://img.shields.io/badge/Docs-opennow.zortos.me-blue?style=for-the-badge" alt="OpenNOW documentation">
  </a>
  <a href="https://discord.gg/8EJYaJcNfD">
    <img src="https://img.shields.io/badge/Discord-Join%20Us-7289da?style=for-the-badge&logo=discord&logoColor=white" alt="Join the OpenNOW Discord">
  </a>
</p>

<p align="center">
  <a href="https://github.com/OpenCloudGaming/OpenNOW-Switch/stargazers">
    <img src="https://img.shields.io/github/stars/OpenCloudGaming/OpenNOW-Switch?style=flat-square" alt="GitHub stars">
  </a>
  <a href="https://github.com/OpenCloudGaming/OpenNOW-Switch/releases">
    <img src="https://img.shields.io/github/downloads/OpenCloudGaming/OpenNOW-Switch/total?style=flat-square" alt="GitHub downloads">
  </a>
  <img src="https://img.shields.io/badge/C%2B%2B-20-00599C?style=flat-square&logo=cplusplus&logoColor=white" alt="C++20">
</p>

## See it in action

<table>
  <tr>
    <td width="50%">
      <img src="docs/screenshots/store.png" alt="OpenNOW Store on Nintendo Switch">
      <br><sub><strong>Store</strong></sub>
    </td>
    <td width="50%">
      <img src="docs/screenshots/library.png" alt="OpenNOW game library on Nintendo Switch">
      <br><sub><strong>Library</strong></sub>
    </td>
  </tr>
  <tr>
    <td width="50%">
      <img src="docs/screenshots/game-details.png" alt="OpenNOW game details on Nintendo Switch">
      <br><sub><strong>Game details</strong></sub>
    </td>
    <td width="50%">
      <img src="docs/screenshots/session-launch.png" alt="OpenNOW cloud session launch on Nintendo Switch">
      <br><sub><strong>Session launch</strong></sub>
    </td>
  </tr>
</table>

> [!WARNING]
> OpenNOW for Nintendo Switch is under active development. Expect occasional
> bugs, rough edges, and platform-specific issues while the client matures.

> [!IMPORTANT]
> OpenNOW is an independent community project and is not affiliated with,
> endorsed by, or sponsored by NVIDIA or Nintendo. A valid GeForce NOW account
> and a modded Nintendo Switch with Homebrew Menu are required.

## Overview

OpenNOW is a native GeForce NOW client for Nintendo Switch homebrew. This
repository contains the Switch launcher, streaming runtime, HOME-screen
shortcut installer, host-side policy tests, and pinned build dependencies.

The client is designed around controller-first catalog browsing and native
WebRTC streaming with low-latency input, H.264 video, Opus audio, NVDEC hardware
decoding, and software-decoder fallback.

## Download

Grab the latest build from
[OpenNOW-Switch Releases](https://github.com/OpenCloudGaming/OpenNOW-Switch/releases/latest).

Install and setup guidance lives in the
[OpenNOW documentation](https://opennow.zortos.me). Nintendo Switch installation
requires custom firmware and Homebrew Menu.

> [!NOTE]
> The application currently retains the inherited `SwitchNOW.nro` filename and
> `sdmc:/switch/SwitchNOW/` data directory for compatibility. Renaming either
> requires a deliberate migration of accounts, settings, shortcuts, and
> recovery data.

## Build from source

The production target requires devkitPro, devkitA64, Switch portlibs, CMake,
Ninja, and the pinned dependencies under `extern/`.

From PowerShell with MSYS2 and devkitPro installed:

```powershell
.\build-switch.ps1
```

From a configured devkitPro MSYS2 shell:

```bash
bash scripts/build-switch-msys2.sh
```

The unified build produces:

```text
build/switch/SwitchNOW.nro
```

It includes NVDEC hardware decoding and the software-decoder fallback; there is
no separate NVDEC build flavor.

Create a release archive with:

```powershell
.\scripts\package-release.ps1 -Version 0.0.6
```

On Linux or in CI, use the equivalent shell entry point:

```bash
bash scripts/package-release.sh 0.0.6
```

Pushing a version tag such as `v0.0.6` runs the Blacksmith release workflow,
publishes both files to GitHub Releases, and generates notes from the changes
since the previous release. The tag must match the version in
`app/src/app_version.hpp` and `CMakeLists.txt`.

## Repository layout

```text
.
├── app/src/                 Launcher, GFN client, streaming, input, and UI
├── app/shortcut/            Per-game shortcut chainloader
├── resources/               RomFS fonts, translations, icons, and UI assets
├── extern/                  Pinned third-party dependencies
├── tests/                   Host-side policy and parsing tests
├── scripts/                 Supported build and packaging entry points
└── tools/
    └── opennow-forwarder-installer/
                             HOME-screen forwarder installer
```

## Development checks

Run the narrowest relevant host test first. A header-only test can be compiled
with:

```bash
g++ -std=c++20 -Wall -Wextra -Werror -Iapp/src tests/<name>.cpp
```

Tests that exercise implementation files must compile those `.cpp` files and
link their host dependencies. Host checks do not replace a devkitA64 Nintendo
Switch build.

## License

OpenNOW Switch is licensed under the [MIT License](LICENSE).

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
