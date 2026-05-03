# esp-smoltcp

Rust **smoltcp** networking stack as an ESP-IDF component, with a
**1:1 BSD-sockets compatibility shim** so existing IDF networking code
(`esp_http_server`, `esp-tls`, `esp-mqtt`, …) works unchanged.

Hardware-verified at **91.15 Mbit/s sustained HTTP throughput on a
100 Mbit/s wired Ethernet link** — ~96 % of practical wire-line max
after framing overhead. Effectively wire-line.

> **Status:** v0.1.0. ESP32-P4 (Waveshare P4-Nano) verified end to end.
> Other RISC-V targets should work but aren't tested yet. ESP-Hosted
> Wi-Fi path is scaffolded; not yet flashed on hardware.

## Why use this?

| | lwIP (default IDF stack) | esp-smoltcp |
|---|---|---|
| Memory safety in the IP stack | C, decades of CVEs | Rust, `#![no_std]` |
| Architecture | Multi-thread + mutex-rich | Single-task poll loop |
| Throughput on 100 Mbit ETH | ~70–90 Mbit/s typical | **~91 Mbit/s** |
| BSD sockets compatibility | Native | Via shim (`--wrap`) |
| `esp_http_server` / `esp-tls` / `esp-mqtt` | Works | **Works unchanged** |
| Code size impact | n/a (already in IDF) | +~80 KiB |
| Behaviour under load | Implementation-defined | Bounded RAM (slab pools), drop counters |

If you have no specific reason to switch, **stay on lwIP** — it's
mature, well-documented, and integrates with everything in IDF. This
component is for projects where the audit/safety story or the
predictable poll model matters more than ecosystem familiarity.

## Quick start

### As a component dependency (recommended)

In your existing IDF project's `main/idf_component.yml`:

```yaml
dependencies:
  datanoisetv/esp_smoltcp: "^0.1"
  datanoisetv/esp_smoltcp_lwip_compat: "^0.1"
```

Then in your `app_main`:

```c
#include "esp_smoltcp.h"

void app_main(void)
{
    nvs_flash_init();
    esp_event_loop_create_default();

    /* Install YOUR esp_eth driver however you want — pins, PHY, all
     * board-specific. esp_smoltcp doesn't care. */
    esp_eth_handle_t eth = my_install_eth_driver();

    /* Bring up smoltcp and attach the eth driver. DHCP starts on
     * its own (or use static IP — Kconfig-controlled). */
    esp_smoltcp_init();
    esp_smoltcp_attach_eth(eth);
    esp_smoltcp_wait_for_ip(ESP_SMOLTCP_IFACE_ETH, 15000);

    /* From here on, BSD sockets just work. */
    httpd_handle_t s;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    httpd_start(&s, &cfg);
    /* … register handlers as usual … */
}
```

A complete working example lives in [`examples/eth_basic/`](examples/eth_basic/).

### Required `sdkconfig.defaults`

Two settings are **mandatory** for the BSD-sockets shim to work:

```
CONFIG_VFS_SUPPORT_SELECT=n     # otherwise select() bypasses our wrap
CONFIG_LWIP_NETIF_LOOPBACK=y    # esp_http_server compile-time check
```

Plus turn on the shim itself:

```
CONFIG_LWIP_COMPAT_ENABLE=y
```

## How it works

```
   IDF networking components (unchanged source)
   esp_http_server | esp-tls | esp-mqtt | mdns | ...
                        |
                  BSD sockets:
                  socket() bind() listen() accept()
                  send() recv() select() getaddrinfo() ...
                        |
                        ▼
              ┌─────────────────────┐
              │ --wrap=lwip_socket  │   linker rewrites every BSD-socket
              │ --wrap=lwip_bind    │   call to __wrap_lwip_*. lwIP stays
              │ --wrap=lwip_select  │   compiled (its headers are needed)
              │ ...                 │   but its socket layer never runs.
              └─────────┬───────────┘
                        │
              esp_smoltcp_lwip_compat
              (FD table, select scan, getaddrinfo, esp_netif shim,
               in-RAM 127.0.0.0/8 loopback for httpd's ctrl socket)
                        │
                        ▼
              ┌─────────────────────┐
              │   esp_smoltcp       │   Single poll task owns smoltcp.
              │   (poll task,       │   Slab-allocated RX frames.
              │    L2 tap, sockets, │   FreeRTOS event-group wakes.
              │    NTP, PTP hooks)  │   Per-iface stats counters.
              └─────────┬───────────┘
                        │
                        ▼
              ┌─────────────────────┐
              │  esp_smoltcp_glue   │   Rust no_std staticlib,
              │  smoltcp 0.12 +     │   riscv32imafc-unknown-none-elf,
              │  DNS resolver +     │   static TX buffer pool, internal-
              │  C FFI              │   RAM-first allocator.
              └─────────┬───────────┘
                        │
                        ▼
              esp_eth_handle_t  /  esp_remote_channel_t
              (app's own driver — installed before attach)
```

The trick: ESP-IDF's `<lwip/sockets.h>` defines `socket()`, `bind()`,
etc. as `static inline` wrappers that call `lwip_socket()`,
`lwip_bind()`. We add `-Wl,--wrap=lwip_socket` etc. to the link, which
redirects every BSD-socket call site to our `__wrap_lwip_*` shim. lwIP's
own socket implementation is still in the binary but unreachable at
runtime. Zero application source changes.

## What works

| Feature | State |
|---|---|
| BSD sockets (`<sys/socket.h>` + `<lwip/sockets.h>`) | ✅ |
| `select()` / `poll()` | ✅ (with `CONFIG_VFS_SUPPORT_SELECT=n`) |
| `getaddrinfo()` / `gethostbyname()` | ✅ minimal A-record resolver |
| `esp_http_server` + chunked encoding + WebSockets | ✅ |
| `esp_https_server` + mbedTLS | ✅ |
| `esp-tls` / `esp_http_client` / `esp-mqtt` | ✅ (BSD sockets only) |
| 127.0.0.0/8 loopback | ✅ in-RAM, never touches the wire |
| IGMPv2 multicast | ✅ |
| ICMP echo (ping) | ✅ |
| L2 frame tap (PTP / LLDP / custom EtherTypes) | ✅ |
| Built-in EMAC (ESP32-P4) | ✅ verified |
| ESP-Hosted-MCU Wi-Fi | scaffolded, **not hardware-verified** |
| PTP IEEE-1588 state machine | tap + HW timestamp wired, state machine TODO |
| mDNS responder | partial (esp_netif shim covers iface enumeration) |

## Performance

Measured on **Waveshare ESP32-P4-Nano** with built-in 100 Mbit/s ETH,
ESP-IDF v6.0, MTU 1500.

```bash
$ curl http://<ip>/dl/200000000 --output foo
100  190M    0  190M    0     0  10.8M     0  --:--:--  0:00:17  --:--:--  10.8M

# server-side (the example app prints this):
I (...) app: dl: 200000000 bytes in 17552922 us = 91.15 Mbit/s
```

| Metric | Value |
|---|---|
| Sustained TCP download | **91.15 Mbit/s** (~96 % of practical max) |
| Round-trip ping (wired) | 0.4–0.7 ms |
| TX failures | 0 / 200 MiB |
| RX frame-pool drops | 0 / 200 MiB |
| Build size impact | ~80 KiB code + ~120 KiB BSS (slab pool + Rust scratch) |

Wire-line max on 100 Mbit/s ETH at MTU 1500 after L2/3/4 framing is
~94.85 Mbit/s. We're at 96 % of that — the remaining ~3 % is
unavoidable framing overhead.

## Components

| Path | Purpose |
|---|---|
| `components/esp_smoltcp_glue/` | Rust `smoltcp` v0.12 staticlib + C FFI. `riscv32imafc-unknown-none-elf` (matches P4 HP-core ABI). Static TX scratch pool. Internal-RAM-first allocator. |
| `components/esp_smoltcp/` | Single-task poll loop, slab-allocated RX frame pool, smoltcp-native socket API, L2 tap, runtime stats, optional SNTP / PTP. Public API: `esp_smoltcp_init()` + `esp_smoltcp_attach_eth()`. |
| `components/esp_smoltcp_lwip_compat/` | Linker `--wrap` BSD-sockets shim. Routes every IDF BSD-socket call to smoltcp without source changes. Provides `127.0.0.0/8` in-RAM loopback for `esp_http_server`'s control socket. |

The three components live together in this repo so they version
together, but each publishes independently to the IDF Component
Registry — apps can depend on whichever subset they need (e.g.
some users may want only `esp_smoltcp` + the smoltcp-native socket
API, skipping the BSD shim).

## Known requirements

- **ESP-IDF v5.5 or v6.0**. v5.4 will probably work but isn't in CI.
- **Rust nightly** with `riscv32imafc-unknown-none-elf` target. Pinned
  in `components/esp_smoltcp_glue/rust-toolchain.toml`. Rustup picks
  it up automatically.
- **Internal SRAM ≥ ~250 KiB free** at startup. The slab pool is 96 KiB
  and the Rust TX scratch is 24 KiB; the rest is for socket buffers.
- **`CONFIG_VFS_SUPPORT_SELECT=n`** in your sdkconfig (see Quick start).

## Documentation

- [`docs/rfc-espressif.md`](docs/rfc-espressif.md) — RFC text for
  upstream Espressif feedback / consideration
- [`docs/rfc-smoltcp.md`](docs/rfc-smoltcp.md) — RFC text for upstream
  smoltcp design review
- [`CHANGELOG.md`](CHANGELOG.md) — release notes
- [`CONTRIBUTING.md`](CONTRIBUTING.md) — what to look at when something
  breaks, architecture rules, perf-tuning rules learned the hard way

## Licensing

Dual-licensed [Apache-2.0](LICENSE-APACHE) OR [MIT](LICENSE-MIT) — pick
whichever fits your downstream. Contributions are dual-licensed under
the same terms; see [`CONTRIBUTING.md`](CONTRIBUTING.md).

## Acknowledgements

- [smoltcp-rs/smoltcp](https://github.com/smoltcp-rs/smoltcp) — the
  upstream Rust IP stack this depends on
- ESP-IDF v6.0 networking team — the eth driver, esp_event, esp_netif,
  and `esp_http_server` were all designed cleanly enough that a
  `--wrap`-based shim could redirect them transparently
- Waveshare for documenting the ESP32-P4-Nano's pin map clearly
