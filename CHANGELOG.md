# Changelog

## [v0.1.0] — 2026-05-04

First public release. Library extracted from the
[esp32p4-smoltcp-tpl](https://github.com/DatanoiseTV/esp32p4-smoltcp-tpl)
template. Hardware-verified at **91.15 Mbit/s sustained on 100 Mbit/s
wired Ethernet** — ~96 % of practical wire-line max.

### Components

- **`esp_smoltcp_glue`** — Rust `smoltcp` v0.12 staticlib + C FFI.
  Targets `riscv32imafc-unknown-none-elf` for the ESP32-P4 HP cores.
- **`esp_smoltcp`** — Single-task poll loop, slab-allocated frame pool,
  smoltcp-native socket API, L2 frame tap, runtime stats, optional
  SNTP / PTP hooks.
- **`esp_smoltcp_lwip_compat`** — Linker `--wrap`-based BSD-sockets
  shim. Routes every IDF networking call (`esp_http_server`, `esp-tls`,
  `esp-mqtt`, …) through smoltcp without source changes. Provides an
  in-RAM 127.0.0.0/8 loopback for esp_http_server's control socket.

### Public API

- `esp_smoltcp_init()` + `esp_smoltcp_attach_eth(eth_handle)` — minimal
  bring-up. Apps install their own `esp_eth` driver (PHY, pins, etc.)
  and hand us the handle.
- `esp_smoltcp_wait_for_ip()`, `esp_smoltcp_get_ipv4()`, …
- `esp_smoltcp_l2_tap()` — raw EtherType frame capture.
- `esp_smoltcp_get_stats()` + `esp_smoltcp_frame_pool_drops()`.

### Known limits

- Only ESP32-P4 hardware-verified so far. Other RISC-V targets should
  work; classic ESP32 (Xtensa) requires a different Rust target spec.
- ESP-Hosted Wi-Fi path is scaffolded but not yet hardware-verified
  on this repo. Track in
  [esp-smoltcp#1](https://github.com/DatanoiseTV/esp-smoltcp/issues/1).
- DNS resolver is single-shot and very minimal (A records only,
  hardcoded primary server). Production users should bring their own.
