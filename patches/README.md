# Patches Applied to the Amazon KVS WebRTC SDK

This directory holds the platform patches that are applied on top of the upstream
[Amazon Kinesis Video Streams WebRTC SDK C](https://github.com/awslabs/amazon-kinesis-video-streams-webrtc-sdk-c)
(included as a submodule at `amazon-kinesis-video-streams-webrtc-sdk-c/`) to make
it work cleanly in the ESP-IDF / FreeRTOS / lwIP environment and to add a few
features that ESP-targeted use cases need but the upstream SDK does not yet ship.

Each patch falls into one of two categories:

1. **Aligned with upstream** — there is (or will be) a corresponding pull request
   on `awslabs/amazon-kinesis-video-streams-webrtc-sdk-c`. When that PR lands, the
   matching local patch can be removed from this directory.
2. **ESP-IDF platform-specific** — the change is specific to ESP-IDF /
   FreeRTOS / lwIP and is not a candidate for upstream. These patches stay
   long-term.

Patches are applied on top of the recorded submodule base (`scripts`/CMake do this
automatically at configure time; to do it by hand):

```bash
cd amazon-kinesis-video-streams-webrtc-sdk-c
git am ../patches/*.patch
cd ..
```

## Patch ledger

Base: upstream **`release-v1.19.0`** (`190a25abce`). All four patches apply
cleanly against this base (`git am`, no fuzz).

| # | Patch | Category | Upstream PR / status |
|---|-------|----------|----------------------|
| 0001 | Added support for SDP re-negotiation flow | aligned | [awslabs#2214](https://github.com/awslabs/amazon-kinesis-video-streams-webrtc-sdk-c/pull/2214) |
| 0002 | SDP renegotiation: apply remote offer to transceiver directions and mark removed tracks inactive | aligned | [awslabs#2214](https://github.com/awslabs/amazon-kinesis-video-streams-webrtc-sdk-c/pull/2214) |
| 0003 | ESP-IDF platform adaptations + robustness — including the `PREFER_DYNAMIC_ALLOCS` / dynamic-signaling-payload work (heap-allocate the payload in `parseSignalingMessage`) and the IceAgent `turnChannelData` heap-allocation | mixed | partial: [awslabs#2146](https://github.com/awslabs/amazon-kinesis-video-streams-webrtc-sdk-c/pull/2146) for the dynamic-allocs portion |
| 0004 | PeerConnection: cap peer Opus encoder at 16kHz mono via `DEFAULT_OPUS_FMTP` | ESP-specific | n/a — narrow-Opus negotiation keeps decode under the 20 ms frame budget on P4 |

> **Not carried as patches:** the upstream `Network.c` uses POSIX `getifaddrs()`,
> which ESP-IDF does not ship, so the ESP-IDF adaptation lives as a full port file
> at [`components/kvs_webrtc/port/Network_esp.c`](../components/kvs_webrtc/port/Network_esp.c)
> (compiled unconditionally; upstream `Network.c` is excluded from the build) rather
> than a patch.

### Notes per patch

**0001 — SDP re-negotiation (basic flow).** Re-offer from the same peer reuses an
active session, or replaces a terminated one. Touches `PeerConnection.c` and
`SessionDescription.c`. Required by the Matter Camera spec. Drops when
**awslabs#2214** lands.

**0002 — SDP re-negotiation (transceiver directions).** Follow-up to 0001 that
applies the remote offer to transceiver directions and marks removed tracks
inactive. Drops together with 0001 once **awslabs#2214** lands.

**0003 — ESP-IDF platform adaptations + robustness.** The largest patch, mixing
two kinds of changes:

  - **Aligned-with-upstream:** the `PREFER_DYNAMIC_ALLOCS` /
    `DYNAMIC_SIGNALING_PAYLOAD` / `USE_DYNAMIC_URL` paths replace huge static
    arrays in the signaling payload and TURN URL fields with optional dynamic
    allocation. This is the same idea as **awslabs#2146** ("Option to use
    dynamic allocations over huge static arrays — limits memory utilization").
    When #2146 lands upstream, this portion is split out and removed.
  - **ESP-IDF-specific (stays):** ConnectionListener custom thread for
    constrained-stack platforms, mbedtls-3.x compatibility shims, robustness
    fixes in `SocketConnection.c`, `Sctp.c` and `LwsApiCalls.c`.

> **SDP buffer cap.** 0003 also lowers `MAX_SESSION_DESCRIPTION_INIT_SDP_LEN`
> 25000 → 12000 to shrink `sendLwsMessage`'s static `encodedMessage` buffer (~13 KB of
> stack). This bound applies **only** to the upstream lws signaling path (ws-off) when
> `PREFER_DYNAMIC_ALLOCS` is off; the ESP ws-on port and the dynamic-allocs path serialize
> the SDP exactly-sized (`serialize → get length → MEMALLOC → serialize`), so they aren't
> capped. Keep this in mind if a peer's SDP grows large (many ICE candidates): on the capped
> path a >12 KB SDP is rejected by the SDK rather than silently truncated.

**0004 — Cap peer Opus encoder at 16 kHz mono via `DEFAULT_OPUS_FMTP`.** When
the browser/Android peer defaults to 48 kHz stereo Opus (Fullband CELT), the
P4 software decoder ends up at ~21 ms / 20 ms frame budget — decode queue
saturates within seconds and every RX audio frame past 50/50 is dropped
(`Audio decode queue full`, `ESP_ERR_NO_MEM`). Narrow `DEFAULT_OPUS_FMTP` to
advertise `maxplaybackrate=16000; sprop-maxcapturerate=16000; stereo=0;
sprop-stereo=0; maxaveragebitrate=24000`, and stop gating the fmtp override
on `isOffer` so the answer path applies the cap too. On bench this drops
per-frame Opus decode from ~21 ms to ~5 ms at session start and eliminates
queue-full / mem errors during the first ~15 s of bidir. ESP-specific
because narrow Opus is a P4 capability trade — keep long-term.

## Patches-removal schedule

The "aligned" patches represent local divergence from upstream and drop as the
matching PRs land:

| Local patch | Removed when |
|-------------|--------------|
| 0001, 0002 | `awslabs#2214` merges |
| 0003 (aligned / dynamic-allocs half) | `awslabs#2146` merges |

Keep this file up-to-date as patches are added, removed, or split.
