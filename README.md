# SwitchNOW

SwitchNOW is a native Nintendo Switch homebrew client for GeForce NOW. It is
distributed as an `.nro`, uses a controller-first Borealis interface and has a
native WebRTC, video, audio and input pipeline.

## Version 1.0

- Unified H.264 streaming client with Tegra X1 NVDEC and software fallback.
- GeForce NOW catalog, owned library, covers, sorting, store selection and
  persistent multi-account sign-in.
- Xbox/Switch controller layouts, touch mouse input and an in-stream keyboard.
- Synchronized Opus audio, configurable stream quality and diagnostics.
- Interface languages: English, Simplified Chinese, Spanish, Russian, Italian,
  French, Polish and Ukrainian.
- In-game language selection remains separate from the launcher language.

The interface language is selected in `Settings > Interface > Language`. Press
`X` to save. Existing installations are migrated automatically from
`sdmc:/switch/OpenNOWSwitch` to `sdmc:/switch/SwitchNOW` on first launch.

## Install

1. Copy `SwitchNOW.nro` and the icons to `sdmc:/switch/SwitchNOW/`.
2. Start Homebrew Menu in application mode by holding `R` while opening an
   installed game.
3. Launch SwitchNOW.

Application data, settings and optional debug logs are stored in
`sdmc:/switch/SwitchNOW/`.

## Build requirements

- Windows with MSYS2 at `C:\msys64`, or an equivalent Unix/MSYS2 shell.
- devkitPro with `devkitA64`, Switch portlibs and CMake/Ninja.
- The dependencies included in `extern/`.

Build from PowerShell:

```powershell
.\build-switch.ps1
```

Output:

```text
build/switch/SwitchNOW.nro
```

Create a conventional release archive:

```powershell
.\scripts\package-release.ps1 -Version 1.0.0
```

## Source layout

```text
app/src/                 Application, GFN API and streaming implementation
resources/               Fonts, icons and RomFS resources
extern/                  Pinned third-party dependencies
tests/                   Host-side policy and parsing tests
scripts/                 Switch build and release packaging
CMakeLists.txt            Switch build definition
build-switch.ps1          Windows build entry point
```

## Privacy

The repository contains no accounts, passwords, tokens, logs or device data.
Saved credentials are created only at runtime on the user's SD card. Password
storage is optional and uses the encrypted local credential vault.

## License and upstream

SwitchNOW is based on the OpenCloudGaming/OpenNOW concepts and uses open-source
components listed in the dependency directories. Review dependency licenses
before redistribution.
