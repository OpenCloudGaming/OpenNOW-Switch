# Optimization fork

Testing optimization strategies on top of
[OpenCloudGaming/OpenNOW-Switch](https://github.com/OpenCloudGaming/OpenNOW-Switch).
Commits are WIP; there is **no released NRO**. Build one locally with
`scripts/build-switch-local.sh` (macOS + devkitPro).

Work lands on `optimization`, then merges to `main`. We track upstream
(read-only remote) and prefer new files over editing upstream ones.

## Patched upstream files

| File | Change | WP |
|---|---|---|
| `main.cpp` | Default log level → INFO | WP-14 |
| `webrtc/diagnostics.cpp` | Parseable per-second + session-end metrics | WP-0 |
| `webrtc/{media,session}.cpp`, `webrtc_session.hpp` | Frame-gap / decode counters | WP-0 |
| `stream/audio/AudioPipeline.{hpp,cpp}` | Audio-buffer occupancy accessor | WP-0 |
| `settings_tab.{cpp,hpp}` | "Debug diagnostics" toggle | WP-0 |
| `stream/ffmpeg/FFmpegVideoDecoder.{cpp,hpp}` | Engage NVDEC (h264 + nvtegra hwaccel via get_format; fixes silent SW fallback) | WP-16 |

New files (not conflict surfaces): `stream/StreamDiagnosticsPolicy.hpp` + its test.

## Fork divergence (not app patches)

`scripts/build-switch-local.sh` (local build), `.gitignore` (keeps private
planning notes out of git), this file.

## Optimization scorecard

Session averages from a diagnostics dump — enable *Settings → App → Interface →
Debug diagnostics*, play, then read `sdmc:/switch/SwitchNOW/stream_trace.log`
(`SUMMARY` line). Lower decode / dropped / gap is better; higher fps / Mbps is
better.

| Build | avg decode ms | present fps | delivered Mbps | dropped | worst gap ms | A/V (subjective) | Notes |
|---|---|---|---|---|---|---|---|
| b63c3fa | 7.0 | 48.0 | 2.79 | 35 (21 lost, 14 overflow) | 1932 | dark, muted, low; laggy input; ~2 s freeze | ⚠ **SW-decode fallback (NVDEC failed)** — not a representative baseline; fix decode first |

After key builds: run the NRO, capture the log, add a row from its `SUMMARY`
line; fill the A/V column by hand.
