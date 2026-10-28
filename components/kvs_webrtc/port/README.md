# kvs_webrtc — ESP-IDF port files

This directory holds the ESP-IDF replacements for upstream KVS WebRTC SDK
sources that need platform-specific implementations.

| Port file | Replaces upstream | Why |
|---|---|---|
| `Tls_esp.c` | `src/source/Crypto/Tls_mbedtls.c` | The same code, except that the server certificate is verified against the ESP-IDF certificate bundle (`esp_crt_bundle_attach()`) instead of a CA file read from `KVS_CA_CERT_PATH`, which ESP devices do not have. Used for TURN over TLS. |
| `Network_esp.c` | `src/source/Ice/Network.c` | The upstream uses POSIX `getifaddrs()` / `freeifaddrs()` for local IP enumeration — ESP-IDF deliberately doesn't ship those. This port uses `esp_netif_get_handle_from_ifkey()` over the standard WiFi STA / WiFi AP / Ethernet keys, and makes `SO_SNDBUF` setsockopt failure non-fatal (lwIP doesn't always honor it). Replaces what used to be patch 0003. |

## Maintenance contract

When the corresponding upstream file evolves on awslabs/develop, the port file
here may need a manual sync. Drift check:

```bash
diff -u amazon-kinesis-video-streams-webrtc-sdk-c/src/source/Crypto/Tls_mbedtls.c \
        components/kvs_webrtc/port/Tls_esp.c
```

Apart from its header (licence, `LOG_CLASS`, the `esp_crt_bundle.h` include),
`Tls_esp.c` differs from upstream only in these places. Bring any other upstream
change over as is.

- CA handling: no `readAndParseCACertificate()`, and `esp_crt_bundle_attach()` in
  place of `mbedtls_ssl_conf_ca_chain()`.
- A failed send closes the session. Part of the record may already be on the
  wire, so the stream can't continue. Upstream ignores the send status.
- No `mbedtls_ssl_set_mtu()`. With DTLS compiled in, mbedTLS applies the MTU to
  stream sessions too and splits each full-size relayed packet into two records.
- A fatal `mbedtls_ssl_read()` error closes the session, so a failed certificate
  check or a server alert fails the TURN connection at once. A TLS 1.3 session
  ticket is not an error.
- The session becomes CONNECTED only from CONNECTING, so a close_notify or a
  fatal error is not undone when the handshake is over.
- The bio callbacks are cast through `void*`, as patch 0001 does for DTLS, for
  the IDF 6 callback types.

Upstream has the same code in all but the first point; these are candidates to
raise there. One upstream behaviour is kept: `freeTlsSession()` calls
`tlsSessionShutdown()` after `mbedtls_ssl_free()`, so it can't send close_notify.
The normal TURN teardown sends it earlier, through `socketConnectionClosed()`.
Sending it from the free would block on a stalled link.

## Gating

- `Tls_esp.c` is gated by `CONFIG_USE_ESP_TLS_FOR_KVS` (default on). The alternative is the upstream `Tls_mbedtls.c`, which needs a CA file at `KVS_CA_CERT_PATH` on the device.
- `Network_esp.c` is **unconditional** — ESP-IDF doesn't ship `getifaddrs()`, so the upstream `Network.c` cannot link on any ESP target. No Kconfig switch.
