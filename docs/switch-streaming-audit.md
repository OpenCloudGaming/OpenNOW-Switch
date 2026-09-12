# Switch streaming and application audit

This audit compares Switch commit `404263a371b2491b5cabb9bd13399aea99bd188f`
with OpenNOW `c1ca71929249788bdef799202eb94660a7799ac2` and OpenNOW-Mac
`d8bbfd26700271c6438f71632cf3b47610dd5615`. It covers the stream transport,
decoder, renderer, audio clock, input capture, cloud launch, account refresh,
catalog, image cache, and settings lifecycle.

The original 54 host tests passed normally and under AddressSanitizer and
UndefinedBehaviorSanitizer. The original source also produced a complete NRO
with the repository's pinned devkitPro container. New tests expose behavior
that those baseline tests did not cover. Host tests and cross-compilation do
not establish Switch frame times, GPU utilization, or successful gameplay.

## Media ownership and recovery

The decoder allocated an uninitialized array of frame pointers, then freed
every slot after partial allocation failure. Its receive path also turned
decoder errors into successful zero-frame submissions, bypassing recovery.
The repair initializes every slot and propagates receive failures while
keeping EAGAIN and EOF as normal no-frame outcomes.
`ffmpeg_video_decoder_test.cpp` injects allocation, send, and receive failures
against the real FFmpeg-facing implementation.

Decoder recovery now has a generation protected by the decoder queue mutex.
An IDR queued for the current generation admits its dependent frames in order.
An older decode completion cannot clear a newer loss event, and a current
decode failure clears queued dependents. Network and UI paths retain the
peer-then-decoder-queue lock order. Decoder submission occurs outside both
locks. Queue capacity, stale-work rejection, keyframe cooldown, and the
software-only decoder reset remain bounded.

The renderer previously retired mappings according to decoded-frame
generations, which can jump when frames are dropped. It now checks GPU
completion fences before reusing mappings, uploads, or mutable command
storage. Each render configuration retains at most eight hardware frames and
holds a reference to the allocation behind each cached mapping. A busy resource
causes a redraw of the previous frame instead of a gameplay-path `waitIdle` or
queue growth.
`gpu_frame_queue_test.cpp` checks ownership with completion tokens, not a GPU.

Frame-size, display-size, and hardware/software transitions now build a complete
replacement configuration before changing the active one. The previous
configuration remains alive until its final draw fence completes. Ownership is
bounded to one active configuration and one replacement or retiring
configuration. A failed allocation preserves the previous image and retries no
more than once every 250 ms. Frames incompatible with that image's layouts are
never copied into them. `gpu_configuration_queue_test.cpp` checks replacement,
failure recovery, and deferred retirement. The actual-renderer host test injects
failures at 26 software and 11 hardware allocation sites during initialization
and replacement, with stubbed GPU completion. GPU fence progress, live SPS
changes, and dock/undock still need hardware acceptance tests.

Audio used one RTP-to-played-sample offset even when output packets were
dropped. A real-pipeline host reproduction accumulated a 40 ms error after
four 10 ms drops. The repaired clock uses a bounded timeline of submitted
sample offsets, RTP timestamps, and SSRCs. Already-queued audio retains its
original position; later submitted audio includes the skipped media time.
The same reproduction reports zero error. Timeline tests also cover SSRC
changes and restart, without claiming measured end-to-end A/V sync on Switch.

## Signaling and bounded startup

WebSocket reassembly now waits for the final fragment, permits interleaved
control frames, and caps a message at 4 MiB. EOF no longer discards a complete
message already received. Upgrade validation checks the response against the
generated challenge and validates Upgrade and Connection tokens. Tests cover
invalid accepts, fragmented JSON, overflow, reconnect, and final-message EOF.

Signaling diagnostics no longer copy arbitrary JSON, SDP, ICE credentials,
or credential-bearing startup URLs into logs. Allowlisted summaries retain
message sizes and numeric metadata. Sender reports use six fixed slots while
preserving the active audio and video sources through SSRC churn.

Transport startup remains limited to 30 seconds. After peer completion, the
client allows another 15 seconds for the first decoded frame instead of
waiting forever. This is distinct from idle-video handling after playback
has begun.

## Reliable input transport

The internal SCTP backend selected by Switch did not retain DATA packets for
retransmission. An actual-source loss test sent two packets, reported the
first missing in repeated gap SACKs, and observed no retransmission. The
application's reliable input channel could therefore lose a discrete key or
button transition despite accepting the send.

Switch now uses the already-pinned usrsctp implementation. Library calls are
serialized globally, but callbacks only enqueue bounded per-association work.
Each owning peer call drains its own DTLS output and application events after
releasing the library lock. A timer serviced by one peer cannot write another
peer's DTLS context. Send buffering is 16 KiB; receive buffering and message
reassembly are each capped at 64 KiB. Output and event queues each have 32
slots. The application retains its existing reliable input-channel profile.

Actual-packet tests cover lost INIT, DATA, and SACK packets, fast retransmit,
duplicate suppression, ordered delivery, separate stream IDs, DCEP mappings,
Forward-TSN, backpressure, fragmentation, timer wrap, and reconnect. A
two-thread test frees one peer's SCTP and DTLS allocation, then advances the
other peer's timers while the runtime remains alive.

The peer wrapper also reference-counts global SRTP initialization. The
original second `peer_init()` failed against host libsrtp, and an unchecked
deinitialization could invalidate the other peer. `PeerRuntime` now balances
initialization through constructor failure as well as ordinary destruction.

The pinned SCTP library retains an idle association-iterator thread even with
`usrsctp_init_nothreads`. ASCONF is disabled and SENDALL is unused; transport
and application callbacks remain on their owning peer paths. Full ThreadSanitizer
is not clean: it reports existing usrsctp lock-order inversions in
`sctputil.c`. A race-only run with deadlock detection disabled passed, but that
is not a full TSan pass. This limitation remains part of the review evidence.

## Input and session lifetime

The 120 ms overlay-chord grace period previously hid the press and release of
quick Plus and Minus taps. Input now records those edges before deferring
transmission. Pending taps expire, delivered taps retain their existing pulse
duration, and a real overlay chord cancels them rather than leaking to the
game. Controller report formats and the Xbox button contract are unchanged.

Keyboard, overlay, NTE automation, and focus loss now share input-capture
transitions. Capture clears pending gameplay pulses and sends neutral
controller reports with bounded retries and 100 ms keepalives. A newly
connected controller receives a neutral report while capture is active.
Tests exercise failed delivery, hotplug, disconnect, and repeated ownership
changes through the production delivery policy.

The startup worker no longer calls a method through a view that may have
been destroyed. It posts callbacks using the lifetime token captured while
the view exists, and only guarded UI callbacks dereference the view.

Cloud launch tracks allocation ownership through cancellation and handoff.
Cancel before a create response causes the worker to delete the returned
allocation; cancel after publication claims it exactly once. Errors after
allocation also release it. `cloud_launch_state_test.cpp` covers both orderings
and concurrent cancellation. This does not implement automatic rig recovery
after network loss.

Minimized queues use the same allocation owner. Their position, progress, and
dialog bindings are updated on the UI thread. Dialog dismissal detaches view
pointers before the closing transition, and Cancel claims the allocation before
that transition starts. Only one launch can queue at a time; stale callbacks
cannot clear a newer queue or launch a stream after an account change. The queue
chip remains available before NVIDIA reports a position, and the configurable
near-ready reminder fires once per launch.

## Accounts and settings

Client-token-only sessions can use the already-implemented renewal grant
instead of being rejected for lacking an OAuth refresh token. Background
renewal updates an existing account without selecting it or recreating a
deleted account. Refresh serialization is separate from the account-file
mutex, so a network request does not hold the lock needed by account actions.
The final write compares the saved credential snapshot and rejects a stale
result after a newer sign-in. Tests use the actual encrypted account store
and a blocked HTTP boundary to verify these cases.

UI session generations reject results from an old account or provider,
including A-to-B-to-A switches. Explicit sign-in and account selection start
a new generation even for the same account; ordinary token updates do not.

Settings recover the atomic writer's backup when primary JSON is missing or
invalid. Invalid video fields are repaired individually instead of erasing
unrelated preferences. JSON integers are range-checked before narrowing, and
legacy audio-gain migration no longer overflows on `INT_MIN`. Existing gain
migrations and valid-primary precedence remain covered.

Provider discovery and cache inspection/deletion run off the UI thread.
Lifetime and session guards protect settings callbacks, and Revert discards
an older proxy-provisioning result. Region caches are associated with the
active session generation. These paths are compiled and policy-tested, not
visually verified on a console.

## Catalog, images, and the certificate screenshot

The authenticated catalog previously stopped after three 120-item requests
and discarded continuation state. It now returns typed 60-game pages and
fetches another page at the loaded end of the 15-card UI pages. Responses are
limited to 4 MiB; malformed pages and cursor cycles preserve the prior
listing rather than masquerading as a successful empty response. A fixture
browses 480 games. Local filtering and sorting explicitly apply to loaded
games, not the entire unseen online catalog.

Covers no longer share Borealis's serial queue with authentication and catalog
requests. A dedicated worker permits one active and 30 pending jobs, drops
obsolete work, and cancels transfers when their image disappears. Saturation
defers admission without cancelling a live widget. Deferred images retry
admission from drawing at most once per 250 ms; network failures keep a valid
fallback without automatic request retries. A regression creates 61 live
images and verifies eventual artwork delivery after saturation. Texture
access stays on the UI thread. The worker shuts down before Borealis destroys
the platform and before curl cleanup. Atomic cache replacement and generation
checks prevent a clear operation from being undone by active or queued work.
Compressed downloads and cached reads are limited to 8 MiB.

The screenshot proves certificate verification failed; it does not identify
the console's clock, firmware, proxy, or certificate chain. The pinned Switch
curl 7.69.1 uses Horizon's libnx TLS backend, not Linux's filesystem trust
store. Horizon includes DigiCert Global Root G3 from version 11.0.0 onward.

The application now packages the public Root G3 certificate as supplemental
older-firmware trust and applies it consistently to HTTP and WebSocket TLS.
Peer, hostname, and certificate-date verification remain enabled. Certificate
errors show the TLS backend and system UTC while omitting URL credentials,
paths, and queries. On 2026-09-12, the public catalog returned HTTP 200 when
verified using the bundled root alone. Local integration tests also reject
a self-signed HTTPS server. Neither result proves the screenshot is cured
on the affected console.

The certificate's DER SHA-256 is
`31AD6648F8104138C738F39EA4320133393E3A18CC02296EF97C2AC9EF6731D0`.
Its source and fingerprint are recorded beside the PEM under `resources/certs`.

## Native NVST

Both reference clients call the protocol NVST. Neither contains an implemented
MVST protocol. Switch's existing `nvstSdp` is metadata alongside its browser
WebRTC answer, not native transport support.

The desktop reference implements native negotiation in
`native/opennow-streamer/crates/opennow-streamer-core/src/nvst_rtsp.rs` and the
video receiver in `opennow-streamer-transport/src/nvst.rs`. The Mac reference
uses `OPN/Stream/NvstBifrostFreeTransport.swift` and
`GFN/NVST/Rtsp/NvstRtspNegotiator.swift`. A Switch port requires these verified
units before it can expose a usable setting:

1. Preserve native allocation endpoints instead of translating them into
   browser signaling URLs, with explicit ownership across cancellation.
2. Implement authenticated WSS `/rtsp` negotiation, bounded CSeq requests,
   keepalive, and teardown. Follow negotiated endpoint and method ordering
   rather than assuming fixed ports or unconditional PLAY.
3. Receive native Mjolnir video with negotiated runtime keys, GCM-8
   authentication, rollover/replay checks, and bounded FEC/reordering.
4. Establish the native audio/control bundle and native input/feedback
   profile. Existing browser channel IDs are not interchangeable with it.
5. Feed authenticated completed H.264 units into the existing decode queue,
   then prove hardware decode, input, reconnect, and packet-loss behavior.

A focused C++ port can reuse protocol knowledge from both projects. Importing
the Rust engine requires proving Horizon support for its runtime, sockets,
crypto, and C ABI; the Swift/Darwin/VideoToolbox implementation is not a Switch
library. No native setting, unused transport scaffolding, or claimed latency
gain is included in this change.

## Reproducible checks and remaining acceptance work

The final integrated source passed 96 reported host checks both normally and
with address/undefined-behavior instrumentation. It produced a complete NRO
with the pinned devkitPro image, and the packaged certificate bytes match the
verified PEM. Independent cross-model review found one live-cover admission
regression, which was fixed and rechecked; no confirmed high findings remained
in that review. The hardware and full-TSan limitations above still apply.

`bash scripts/test-host.sh` runs the complete host suite, including real
FFmpeg-facing failure injection, local HTTP/TLS transfers, image-worker
cancellation, settings/account persistence, and production input policies.
`bash scripts/test-streaming-host.sh` remains the narrower media/input suite.
The GitHub host workflow runs the complete suite on pull requests and `main`.
`OPENNOW_SANITIZERS=address,undefined bash scripts/test-host.sh` enables the
address and undefined-behavior checks. The SCTP test builds a temporary host
copy of the pinned library and removes only its two forced Switch platform
defines; the source dependency in the repository is not modified by tests.

The supported Switch build remains `bash scripts/build-switch-msys2.sh` in
the configured devkitPro environment. A successful NRO build is necessary
but cannot verify GPU fences, controller feel, or live NVIDIA sessions.

Hardware acceptance still includes packet loss during IDR decode, slow GPU
completion, repeated NVDEC/software start-stop, audio congestion, input
capture under failed sends, browsing while covers stall, account changes
during fetches, cache clear during downloads, and exit during a stalled
request. Check the affected console's firmware and UTC time when reproducing
the certificate failure.

Remaining product limitations include a decoded-image pixel budget and
disk-cache quota, offline catalog freshness, complete live
localization, and automatic cloud-rig recovery. Rig recovery must separate
local teardown from remote DELETE and transfer allocation ownership before
changing today's stale-session cleanup. None of these is claimed complete.
