# Native WebRTC Stack

OpenNOW Switch uses a native Horizon OS media path. A browser, Electron, or
desktop WebRTC runtime is not available on Switch homebrew.

## Selected Stack

The viable embedded WebRTC base is `sepfy/libpeer`.

- C API and BSD sockets fit libnx better than Google WebRTC.
- Uses mbedTLS for DTLS, libsrtp for SRTP, and usrsctp for data channels.
- Has H264 RTP depacketization callbacks that can feed the Switch decode path.
- License is MIT.

`libdatachannel` remains the fallback if libpeer cannot negotiate NVIDIA's GFN
SDP shape or data-channel requirements. It is more complete but heavier and has
more C++17/platform surface to port.

## Current Port Status

Working:

- `switch-mbedtls`, `switch-ffmpeg`, and `switch-jansson` are installed through
  devkitPro packages.
- OpenNOW now builds and links a native mbedTLS WSS client for Switch.
- Vendored libpeer research build has mbedTLS DTLS-SRTP compiling for Switch
  after adding:
  - `MBEDTLS_SSL_DTLS_SRTP`
  - `MBEDTLS_ENTROPY_HARDWARE_ALT`
  - Switch entropy through `randomGet`
  - no platform entropy
  - no mbedTLS net/timing modules
- libsrtp compiles for Switch after including `arpa/inet.h` before byte-order
  helpers.

Blocking:

- usrsctp does not compile against libnx/newlib yet. It expects several BSD/POSIX
  headers and types that are not exposed by Switch homebrew:
  - `sys/uio.h`
  - `netinet/in_systm.h`
  - `netinet/ip.h`
  - `ifaddrs.h`
  - BSD aliases such as `u_long`, `u_int`, `u_short`, `u_char`

Until usrsctp is ported, WebRTC data channels are not real. GFN input normally
rides over the WebRTC data channel, so full playability is blocked even if video
RTP negotiation proceeds.

## Implementation Order

1. Finish the usrsctp libnx compatibility layer.
2. Vendor libpeer and its patched dependencies into `extern/libpeer`.
3. Add an OpenNOW `GfnWebRtcClient` wrapper around `PeerConnection`.
4. Use the native WSS client to connect NVIDIA signaling, send peer info, read
   the remote offer, and send the local answer plus `nvstSdp`.
5. Feed H264 RTP output into FFmpeg NVTEGRA decode.
6. Render decoded frames through deko3d.
7. Decode Opus and output through Audren.
8. Send controller packets through the negotiated data channel.

No UI should present "Play" as available until all steps above are linked and
runtime-tested on hardware.
