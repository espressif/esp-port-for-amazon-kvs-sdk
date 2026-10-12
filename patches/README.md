# Patches Applied to the Amazon KVS WebRTC SDK

This directory holds the platform patches that are applied on top of the upstream
[Amazon Kinesis Video Streams WebRTC SDK C](https://github.com/awslabs/amazon-kinesis-video-streams-webrtc-sdk-c)
(included as a submodule at `amazon-kinesis-video-streams-webrtc-sdk-c/`) to make
it work cleanly in the ESP-IDF / FreeRTOS / lwIP environment and to add a few
features that ESP-targeted use cases need but the upstream SDK does not yet ship.

Each patch falls into one of two categories:

1. **Aligned with upstream**: there is (or will be) a corresponding pull request
   on `awslabs/amazon-kinesis-video-streams-webrtc-sdk-c`. When that PR lands, the
   matching local patch can be removed from this directory.
2. **ESP-IDF platform-specific**: the change is specific to ESP-IDF /
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

Base: upstream `develop` at **awslabs#2429** (docs). Since 1.20.0 it adds, among
others:

- awslabs#2399: negotiated data channels with an explicit stream id;
- #2400: `iceServersFinal`;
- #2401: freeing the signaling client during a reconnect;
- #2417: the lws service-loop wake after a send;
- #2425: fetch, disconnect and delete refused while a free runs, and messages received
  during a free dropped.

All three patches apply cleanly against this base (`git am`, no fuzz). 0001's
`LwsApiCalls.c` part sits on top of #2401's reconnect and receive-worker code and #2425's
message drop. The SDP-renegotiation patches were dropped earlier (that flow is upstream
as awslabs#2214, in 1.20.0). 0005 is upstream as awslabs#2392 and 0004 as
awslabs#2385, so both are dropped. 0003 was dropped as unnecessary; see its note.
An earlier 0006, the `pSeenTransceivers` fix, is upstream as awslabs#2415. The
current 0006 is the WSS ping interval guard. The numbers of the remaining patches
are unchanged.

| # | Patch | Category | Upstream PR / status |
|---|-------|----------|----------------------|
| 0001 | ESP-IDF platform adaptations + robustness, including the `PREFER_DYNAMIC_ALLOCS` / dynamic-signaling-payload work (heap-allocate the payload in `parseSignalingMessage`) and the IceAgent `turnChannelData` heap-allocation | mixed | partial: [awslabs#2146](https://github.com/awslabs/amazon-kinesis-video-streams-webrtc-sdk-c/pull/2146) for the dynamic-allocs portion |
| 0002 | PeerConnection: cap peer Opus encoder at 16kHz mono via `DEFAULT_OPUS_FMTP` | ESP-specific | n/a; narrow-Opus negotiation keeps decode under the 20 ms frame budget on P4 |
| ~~0003~~ | Ice: accept already-connected TCP in `socketConnectionIsConnected` via `getpeername` | n/a | **dropped**: not needed; upstream's check works on lwIP (see note) |
| ~~0004~~ | Crypto: take the RNG from PSA when mbedTLS has no entropy module (ESP-IDF 6 / TF-PSA-Crypto builds) | aligned | **upstream** as [awslabs#2385](https://github.com/awslabs/amazon-kinesis-video-streams-webrtc-sdk-c/pull/2385); dropped |
| ~~0005~~ | Crypto: hash the DTLS certificate through `mbedtls_md()` | portability | **upstream** as [awslabs#2392](https://github.com/awslabs/amazon-kinesis-video-streams-webrtc-sdk-c/pull/2392); dropped |
| 0006 | Signaling: let the build override the WSS ping interval (`#ifndef` guard around `SIGNALING_SERVICE_WSS_PING_PONG_INTERVAL_IN_SECONDS`) | aligned | candidate: two-line guard, not yet proposed upstream |
| 0010 | Signaling: end the fetch and GET_TOKEN back-offs on shutdown | aligned | not yet raised upstream |

> **Not carried as patches:** the upstream `Network.c` uses POSIX `getifaddrs()`,
> which ESP-IDF does not ship, so the ESP-IDF adaptation lives as a full port file
> at [`components/kvs_webrtc/port/Network_esp.c`](../components/kvs_webrtc/port/Network_esp.c)
> (compiled unconditionally; upstream `Network.c` is excluded from the build) rather
> than a patch.

### Notes per patch

**0001: ESP-IDF platform adaptations + robustness.** The largest patch, mixing
two kinds of changes:

  - **Aligned-with-upstream:** the `PREFER_DYNAMIC_ALLOCS` /
    `DYNAMIC_SIGNALING_PAYLOAD` / `USE_DYNAMIC_URL` paths replace huge static
    arrays in the signaling payload and TURN URL fields with optional dynamic
    allocation. This is the same idea as **awslabs#2146** ("Option to use
    dynamic allocations over huge static arrays: limits memory utilization").
    When #2146 lands upstream, this portion is split out and removed.
  - **ESP-IDF-specific (stays):** ConnectionListener custom thread for
    constrained-stack platforms, mbedtls-3.x compatibility shims, robustness
    fixes in `SocketConnection.c` and `Sctp.c`, and in `LwsApiCalls.c` the
    reconnect/receive thread stacks (on top of #2401's `startReconnectHandler()`
    and counted receive workers), the payload free on every receive-wrapper
    free, and the longer API-call timeouts and TCP keepalive.

> **lws callback rewrite removed.** 0001 used to carry the libwebsockets callback
> and send-path changes of awslabs#2134 (partial HTTP body writes, a ping timer,
> close-status handling, fragment tracking, a WRITEABLE / `writeLwsData` rewrite,
> forced ALPN). They are gone: ALPN `http/1.1` is upstream since v1.20.0, lws
> buffers partial writes itself on http/1.1, and the send-path rewrite returned
> from `awaitForResponse` before the response arrived. Checked end to end on the
> ws-off path under ESP-EMU (`webrtc_classic`, P4 + C6): signaling, answer, ICE,
> video and the data channel behave the same with and without them, and no short
> write occurred on either. That run needed two ws-off fixes:
> - waking the lws service loop with `lws_cancel_service()` after queuing a send,
>   upstream as awslabs#2417 and in the pinned base;
> - no file cache on ws-off (upstream `FileCache.c` puts 32 cache entries on the
>   stack), not part of this patch set yet.

> **SDP buffer cap.** 0001 also lowers `MAX_SESSION_DESCRIPTION_INIT_SDP_LEN`
> 25000 → 12000 to shrink `sendLwsMessage`'s static `encodedMessage` buffer (~13 KB of
> stack). This bound applies **only** to the upstream lws signaling path (ws-off) when
> `PREFER_DYNAMIC_ALLOCS` is off; the ESP ws-on port and the dynamic-allocs path serialize
> the SDP exactly-sized (`serialize → get length → MEMALLOC → serialize`), so they aren't
> capped. Keep this in mind if a peer's SDP grows large (many ICE candidates): on the capped
> path a >12 KB SDP is rejected by the SDK rather than silently truncated.

**0002: Cap peer Opus encoder at 16 kHz mono via `DEFAULT_OPUS_FMTP`.** When
the browser/Android peer defaults to 48 kHz stereo Opus (Fullband CELT), the
P4 software decoder ends up at ~21 ms / 20 ms frame budget, so the decode queue
saturates within seconds and every RX audio frame past 50/50 is dropped
(`Audio decode queue full`, `ESP_ERR_NO_MEM`). Narrow `DEFAULT_OPUS_FMTP` to
advertise `maxplaybackrate=16000; sprop-maxcapturerate=16000; stereo=0;
sprop-stereo=0; maxaveragebitrate=24000`, and stop gating the fmtp override
on `isOffer` so the answer path applies the cap too. On bench this drops
per-frame Opus decode from ~21 ms to ~5 ms at session start and eliminates
queue-full / mem errors during the first ~15 s of bidir. ESP-specific
because narrow Opus is a P4 capability trade; keep long-term.

**0010: End the fetch and GET_TOKEN back-offs on shutdown.** A free sets the
client's `shutdown` flag, and so does `app_webrtc_terminate()` through the signaling
`abort` op. Two sleeps ignored it: the back-off between `signalingClientFetchSync()`
attempts (up to about 32 s with full jitter) and the GET_TOKEN state's retry sleep (up
to 10 s). Both now sleep in 100 ms steps and stop on `shutdown`. The fetch then returns
`STATUS_SIGNALING_FREE_IN_PROGRESS`, as for a fetch refused during a free (#2425).
GET_TOKEN fetches no token, and the state machine stops at its next `shutdown` check.
A candidate for upstream, next to #2425.

**0003: Accept already-connected TCP in `socketConnectionIsConnected`** (dropped).
It added a `getpeername()` check after upstream's re-`connect()` probe, on the
belief that lwIP reports a connected TCP socket as `EALREADY`. It does not:
- lwIP returns `EISCONN` for an established socket (`tcp_connect()` on a pcb
  that is not CLOSED), which upstream already accepts. `EALREADY` means the
  connect is still in progress, and the TURN state machine probes again later.
  This is the same in the lwIP of ESP-IDF 5.5, 6.0, 6.1 and master.
- newlib prints `EALREADY` (120) as "Socket already connected", so the SDK's
  "socket connection check failed" warning for a connect in progress reads like
  a connected socket being rejected. `EISCONN` (127) is "Socket is already
  connected".
- lwIP's `getpeername()` succeeds from SYN_SENT on, so the patch reported a
  connect still in progress as connected.

Checked under ESP-EMU with the decision made as upstream makes it: TURN
TCP/TLS sockets got `EISCONN` on the first probe, or after about 0.7 s when
caught mid-connect, and viewers streamed. TLS TURN failed the same way with
and without the patch; that was the ESP TLS layer, not this check.

**0004: Take the RNG from PSA when mbedTLS has no entropy module** (upstream
as awslabs#2385; kept here for the history). ESP-IDF 6 sets
`MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG`, so mbedTLS compiles its entropy module out
and `mbedtls_entropy_*` / `mbedtls_ctr_drbg_*` do not link. As merged, the DTLS
and TLS sessions keep their entropy context and CTR-DRBG only before mbedTLS 4;
on mbedTLS 4 the SDK takes all its randomness from PSA.

**0005: Fingerprint through the message-digest layer** (upstream as
awslabs#2392, which also moves the TURN MD5 through `mbedtls_md()`; kept here for
the history). mbedTLS 4
(TF-PSA-Crypto) moved `mbedtls_sha256()` into the private header
`mbedtls/private/sha256.h`, and the builtin SHA-256 driver is only compiled when
no platform PSA accelerator is registered. On ESP-IDF v6 with
`CONFIG_MBEDTLS_HARDWARE_SHA=y` (the default on every target with a SHA
peripheral) the prototype is still visible, so `dtlsCertificateFingerprint()`
compiles and then fails at link with `undefined reference to 'mbedtls_sha256'`.
`mbedtls_md()` is public API in mbedTLS 2, 3 and 4 and dispatches to whatever
driver is active; `pMdInfo` was already being fetched two lines above for
`mbedtls_md_get_size()`, so this also removes the `MBEDTLS_BEFORE_V3` fork.

**0006 — Let the build override the WSS ping interval.** On battery targets the
signaling ping is a power knob: each ping is a radio transmit. The patch guards
the upstream `SIGNALING_SERVICE_WSS_PING_PONG_INTERVAL_IN_SECONDS` default (10 s)
with `#ifndef`, and `components/signaling/kvs_signaling/CMakeLists.txt` defines it
from `CONFIG_KVS_SIGNALING_WSS_PING_INTERVAL_SEC` on the libwebsockets path, so
both signaling backends use one cadence. `Signaling.c` derives
`secs_since_valid_ping` from the same constant, so the two move together. With
nothing defined the default stays 10 s.

## Patches-removal schedule

The "aligned" patches represent local divergence from upstream and drop as the
matching PRs land:

| Local patch | Removed when |
|-------------|--------------|
| 0001 (aligned / dynamic-allocs half) | `awslabs#2146` merges |
| ~~0004~~ | dropped: upstream as awslabs#2385 |
| ~~0005~~ | dropped: upstream as awslabs#2392 |
| 0006 | the build override is accepted upstream |

Keep this file up-to-date as patches are added, removed, or split.
