# kvs_webrtc — ESP-IDF port files

This directory holds the ESP-IDF replacements for upstream KVS WebRTC SDK
sources that need platform-specific implementations.

| Port file | Replaces upstream | Why |
|---|---|---|
| `Tls_esp.c` | `src/source/Crypto/Tls_mbedtls.c` | TLS via ESP-IDF's `esp-tls` instead of direct mbedTLS API calls. Gives us esp-tls's connection management + the consistent error semantics ESP-IDF users expect. |
| `Network_esp.c` | `src/source/Ice/Network.c` | The upstream uses POSIX `getifaddrs()` / `freeifaddrs()` for local IP enumeration — ESP-IDF deliberately doesn't ship those. This port uses `esp_netif_get_handle_from_ifkey()` over the standard WiFi STA / WiFi AP / Ethernet keys, and makes `SO_SNDBUF` setsockopt failure non-fatal (lwIP doesn't always honor it). Replaces what used to be patch 0003. |

## Maintenance contract

When the corresponding upstream file evolves on awslabs/develop, the port file
here may need a manual sync. Drift check:

```bash
diff -u amazon-kinesis-video-streams-webrtc-sdk-c/src/source/Crypto/Tls_mbedtls.c \
        components/kvs_webrtc/port/Tls_esp.c
```

The port file does not 1:1 mirror the upstream — it implements the same TLS
abstraction over `esp_tls_*` rather than `mbedtls_ssl_*` — but the public
TLS_* contract from `Tls.h` is preserved.

## Gating

- `Tls_esp.c` is gated by `CONFIG_USE_ESP_TLS_FOR_KVS` (default on) — the alternative is direct mbedTLS API calls via the upstream `Tls_mbedtls.c`. Both work on ESP; the choice is about certificate-management strategy.
- `Network_esp.c` is **unconditional** — ESP-IDF doesn't ship `getifaddrs()`, so the upstream `Network.c` cannot link on any ESP target. No Kconfig switch.
