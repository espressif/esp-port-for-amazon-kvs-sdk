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

Base: upstream `develop` at **`4dd058d5`** (the 1.20.0 version-bump commit,
awslabs#2382; no release tag yet). All five patches apply cleanly against this
base (`git am`, no fuzz). The SDP-renegotiation patches were dropped here — that
flow is now upstream (awslabs#2214, in 1.20.0).

| # | Patch | Category | Upstream PR / status |
|---|-------|----------|----------------------|
| 0001 | ESP-IDF platform adaptations + robustness — including the `PREFER_DYNAMIC_ALLOCS` / dynamic-signaling-payload work (heap-allocate the payload in `parseSignalingMessage`) and the IceAgent `turnChannelData` heap-allocation | mixed | partial: [awslabs#2146](https://github.com/awslabs/amazon-kinesis-video-streams-webrtc-sdk-c/pull/2146) for the dynamic-allocs portion |
| 0002 | PeerConnection: cap peer Opus encoder at 16kHz mono via `DEFAULT_OPUS_FMTP` | ESP-specific | n/a — narrow-Opus negotiation keeps decode under the 20 ms frame budget on P4 |
| 0003 | Ice: accept already-connected TCP in `socketConnectionIsConnected` (lwIP reports `EALREADY`, not `EISCONN`; use `getpeername`) | ESP-IDF/lwIP-specific | candidate — low-priority portability PR to upstream; drop if it lands |
| 0004 | Crypto: take the RNG from PSA when mbedTLS has no entropy module (`MBEDTLS_HAS_ENTROPY` gate; ESP-IDF 6 / TF-PSA-Crypto builds) | aligned | [awslabs#2385](https://github.com/awslabs/amazon-kinesis-video-streams-webrtc-sdk-c/pull/2385) (approved, awaiting merge) |
| 0005 | Crypto: hash the DTLS certificate through `mbedtls_md()` instead of `mbedtls_sha256()` in `dtlsCertificateFingerprint` | portability | candidate — not yet submitted; needed by any mbedTLS 4 build with a platform PSA hash driver |

> **Not carried as patches:** the upstream `Network.c` uses POSIX `getifaddrs()`,
> which ESP-IDF does not ship, so the ESP-IDF adaptation lives as a full port file
> at [`components/kvs_webrtc/port/Network_esp.c`](../components/kvs_webrtc/port/Network_esp.c)
> (compiled unconditionally; upstream `Network.c` is excluded from the build) rather
> than a patch.

### Notes per patch

**0001 — ESP-IDF platform adaptations + robustness.** The largest patch, mixing
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

> **SDP buffer cap.** 0001 also lowers `MAX_SESSION_DESCRIPTION_INIT_SDP_LEN`
> 25000 → 12000 to shrink `sendLwsMessage`'s static `encodedMessage` buffer (~13 KB of
> stack). This bound applies **only** to the upstream lws signaling path (ws-off) when
> `PREFER_DYNAMIC_ALLOCS` is off; the ESP ws-on port and the dynamic-allocs path serialize
> the SDP exactly-sized (`serialize → get length → MEMALLOC → serialize`), so they aren't
> capped. Keep this in mind if a peer's SDP grows large (many ICE candidates): on the capped
> path a >12 KB SDP is rejected by the SDK rather than silently truncated.

**0002 — Cap peer Opus encoder at 16 kHz mono via `DEFAULT_OPUS_FMTP`.** When
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

**0003 — Accept already-connected TCP in `socketConnectionIsConnected`.** The
function re-`connect()`s the socket and treats only `EISCONN` as connected. On
lwIP an already-connected TCP socket returns `EALREADY`, not `EISCONN`, so a
live TCP/TLS TURN channel is wrongly declared dead — TURN stalls and, on
UDP-blocked networks (TLS TURN only), ICE never gets a relay candidate. Fix
confirms with `getpeername()` after the existing `EISCONN` fast path (POSIX
builds unaffected). Worth upstreaming as portability hardening.

**0004 — Take the RNG from PSA when mbedTLS has no entropy module.** ESP-IDF 6
sets `MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG`, so mbedTLS compiles its entropy module
out and `mbedtls_entropy_*` / `mbedtls_ctr_drbg_*` do not link. The patch keys
every entropy user on a single `MBEDTLS_HAS_ENTROPY` switch (defined in
`Include_i.h`): entropy builds keep the CTR-DRBG exactly as before; entropy-less
builds take the RNG from PSA (`mbedtls_psa_get_random`), with an idempotent
`psa_crypto_init()` in `createCertificateAndKey()` since `createRtcCertificate()`
is public API and can run before `initKvsWebRtc()`. A guard at the top of
`Dtls.h` makes the layout switch tamper-proof: including it without
`Include_i.h` is a compile error rather than a silently smaller
`struct __DtlsSession`. Submitted upstream as
[awslabs#2385](https://github.com/awslabs/amazon-kinesis-video-streams-webrtc-sdk-c/pull/2385).

**0005 — Fingerprint through the message-digest layer.** mbedTLS 4
(TF-PSA-Crypto) moved `mbedtls_sha256()` into the private header
`mbedtls/private/sha256.h`, and the builtin SHA-256 driver is only compiled when
no platform PSA accelerator is registered. On ESP-IDF v6 with
`CONFIG_MBEDTLS_HARDWARE_SHA=y` (the default on every target with a SHA
peripheral) the prototype is still visible, so `dtlsCertificateFingerprint()`
compiles and then fails at link with `undefined reference to 'mbedtls_sha256'`.
`mbedtls_md()` is public API in mbedTLS 2, 3 and 4 and dispatches to whatever
driver is active; `pMdInfo` was already being fetched two lines above for
`mbedtls_md_get_size()`, so this also removes the `MBEDTLS_BEFORE_V3` fork.

## Patches-removal schedule

The "aligned" patches represent local divergence from upstream and drop as the
matching PRs land:

| Local patch | Removed when |
|-------------|--------------|
| 0001 (aligned / dynamic-allocs half) | `awslabs#2146` merges |
| 0004 | [awslabs#2385](https://github.com/awslabs/amazon-kinesis-video-streams-webrtc-sdk-c/pull/2385) merges |
| 0005 | upstream stops calling `mbedtls_sha256()` directly (or mbedTLS re-exports it) |

Keep this file up-to-date as patches are added, removed, or split.
