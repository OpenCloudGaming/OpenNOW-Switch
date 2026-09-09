# Bitrate negotiation: source and validation notes

## Setting lifetime and units

The maximum bitrate setting is a ceiling in decimal kilobits per second, not a
constant-rate encoder target. The menu cycles through 8000, 12000, 16000, 20000,
and 25000 kbps. Safe, Balanced, and Quality presets select 8000, 12000, and
20000 kbps respectively. `SaveStreamSettings` persists `bitrate_kbps` unchanged
for these values using the existing temporary-file replacement mechanism;
`LoadStreamSettings` does not impose a 7000 kbps cap.

Save the setting **before launching the next cloud session**. CloudMatch reads
settings in `GfnClient::StartSession`, and `WebRtcSession` takes its own settings
snapshot when constructed. Later server offers reuse that snapshot for answers;
saving settings does not send a runtime bitrate update or change an existing
session. A new launch ensures that CloudMatch and WebRTC use the same saved cap.
The settings save notification already says changes apply to the next stream.

## Wire path

| Stage | Implementation | Value |
| --- | --- | --- |
| Persistence | `stream_settings.cpp` | JSON integer `bitrate_kbps` |
| CloudMatch launch | `gfn/cloud_session_protocol.cpp`, `BuildSessionBody` | `requestedStreamingFeatures.maxBitrateKbps` |
| WebRTC answer | `webrtc/negotiation.cpp`, `AdaptAnswerSdpToOffer` | Video `b=AS:<bitrate_kbps>` |
| NVST answer | `webrtc/nvst_sdp.cpp`, `BuildNvstSdp` | `vqos.bw.maximumBitrateKbps` |
| Subsequent offers | `webrtc/negotiation.cpp`, `handle_signaling_message` | Rebuilds both answers from the same session snapshot |

NVST uses a 4000 kbps minimum and starts at `max(4000, maximum / 4)`. For the
menu choices, startup rates are 4000, 4000, 4000, 5000, and 6250 kbps. Motion
quality changes FEC and packet pacing, not these bitrate values. Hand-edited
values below 4000 are outside the menu range: NVST floors them to 4000 while
CloudMatch and standard SDP retain the saved value.

Commit `dce9743` (#30) already added the CloudMatch streaming features and
aligned NVST startup/minimum rates and attributes with the web client. In
particular, it removed the extra peak, bandwidth-estimation, and GRC bitrate
attributes. This audit does not undo that fix or raise the minimum to force a
constant rate. The answer advertises NACK/PLI only, not REMB or transport-wide
congestion-control feedback that libpeer does not implement.

## Reference comparison

References were inspected outside this checkout at these revisions:

- `OpenCloudGaming/OpenNOW` at `a9180d376ed1e643bd150364b8d21beba6d11784`:
  `opennow-stable/src/renderer/src/platforms/gfn/sdp/nvstOffer.ts` uses the same
  4000 kbps minimum and quarter-cap startup formula. Its bitrate section
  documents removal of the extra fork-only attributes after throttling near
  the minimum. This is consistent with the fix already present in #30.
- `OpenCloudGaming/OpenNOW-Mac` at
  `4f279edc7b0137f2691abae0c9ad90d49fbc84a5`: its WebRTC NVST builder in
  `GFN/NVST/SDP/NVSTSessionDescription.swift` uses different tuning, including a
  70% startup rate. Its independent RTSP transport in
  `OPN/Stream/NvstBifrostFreeTransport.swift` documents a measured startup-rate
  plateau and starts at the configured ceiling. Its runtime controls depend
  on libwebrtc or native NVST APIs. Those transport-specific results do not
  establish that Switch should copy those values or protocols.

## What a 7 Mbps observation proves

The overlay calculates Mbps from completed encoded video access-unit bytes,
not total network throughput or the negotiated maximum. Content complexity,
server adaptation, loss before assembly, and frame delivery can all affect
that number. Static inspection and host tests found no 7 Mbps constant or
remaining loss of a menu-selected cap in the persistence/launch/NVST path.
They cannot establish why a particular live session stays near 7 Mbps.

Matching the web client's SDP does not reproduce its receiver feedback stack.
Before this update, libpeer sent NACK/PLI but no RTCP receiver reports, REMB,
or transport-wide congestion-control feedback. NVST still announces
`bwe.useOwdCongestionControl:1` and
`vqos.bw.txRxLag.minFeedbackTxDeltaMs:200`. Missing receiver telemetry is a
credible reason for a server to remain near its startup rate rather than
ramp to the ceiling; preserving settings alone cannot fix that gap. A
hand-edited 28000 kbps maximum, for example, starts at 7000 kbps, though none
of the menu choices starts at exactly 7000.

Mac's native `GFN/NVST/BifrostFree/NvstQosReport.swift` and
`OPN/Stream/NvstBifrostFreeVideo.swift` tie those OWD attributes to command
`0x0207` feedback on a native control channel. That is evidence to investigate
feedback, not evidence that its proprietary report format belongs on the
Switch WebRTC data channel.

This update adds protected compound RTCP receiver reports with video packet
loss, extended sequence numbers, jitter, and sender-report timing, once per
second after video reception starts. Report state is fixed-size, transmission
failures are rate-limited, and malformed inbound RTCP is rejected before
callbacks. The effect on GFN's bitrate ramp still needs measurement; this does
not implement proprietary OWD reports, REMB, or TWCC. Disabling OWD or
advertising an unimplemented feedback capability would not supply that
missing feedback.

For a controlled hardware check, save a cap, start a new session, and use
opt-in diagnostics to compare the launch settings and the adapted standard
and NVST answers with the measured bitrate during repeatable high-motion
gameplay. Test more than one cap under comparable network conditions. Treat
diagnostic captures as sensitive: do not publish raw signaling SDP, account
data, or session credentials. A low observed rate alone is not a reason to
disable adaptation, overload the connection, or raise the startup rate.

## Host regression

`tests/stream_bitrate_pipeline_test.cpp` substitutes only the application
storage location with a unique temporary directory. It exercises the real
save/load implementations and CloudMatch/NVST builders for all selectable
bitrates, frame rates, and motion-quality modes, presets, a custom bitrate,
and legacy settings without a bitrate field. It also checks that saving a
new value does not mutate an existing settings snapshot. It does not execute
the networked `WebRtcSession` lifecycle or validate server throughput.

```sh
g++ -std=c++20 -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
  -Iapp/src tests/stream_bitrate_pipeline_test.cpp \
  app/src/stream_settings.cpp app/src/localization.cpp \
  app/src/gfn/cloud_session_protocol.cpp app/src/gfn/shared.cpp \
  app/src/webrtc/nvst_sdp.cpp -Wl,--gc-sections -ljansson \
  -o /tmp/stream_bitrate_pipeline_test
/tmp/stream_bitrate_pipeline_test
```

The section-garbage-collection flags exclude unrelated network entry points
from the host executable. This is a host behavior check, not an NRO build or
a substitute for an account-backed test on Switch hardware.
