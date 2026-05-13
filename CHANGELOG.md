# Changelog

## [v0.2.0] — unreleased

On the `feat/v0.2-vfs-and-netif` branch. Builds clean against
ESP-IDF v6.0; hardware validation pending.

### Added

- **VFS-registered socket layer.** `esp_smoltcp_vfs_register()` claims
  the BSD-socket FD range `[LWIP_SOCKET_OFFSET, MAX_FDS)` via
  `esp_vfs_register_fd_range()` after lwIP, taking over its FD
  ownership. IDF's `select()` now dispatches to us through the
  registered `esp_vfs_select_ops_t` instead of bypassing our wrap via
  a function-pointer table. The `__wrap_lwip_*` shims are still the
  underlying implementations.
- **Runtime hostname** via `esp_netif_set_hostname()` — per-iface
  buffer on `struct shim_netif`, seeded from `CONFIG_LWIP_COMPAT_HOSTNAME`
  at boot. The setter is no longer a no-op.

### Changed

- **`CONFIG_VFS_SUPPORT_SELECT=y` (the IDF default) now works.** The
  v0.1 requirement to set it `=n` is removed; the README, CONTRIBUTING,
  per-component READMEs and `examples/eth_basic/sdkconfig.defaults` are
  updated accordingly.

### Example fixes

Two pre-existing example-build bugs surfaced and were fixed on this
branch (unrelated to v0.2 changes themselves):

- `examples/eth_basic/main/idf_component.yml` now depends on
  `espressif/ip101 ^1.0.0` — the IP101 PHY moved out of the IDF tree
  in v6.0.
- `examples/eth_basic/main/main.c` now includes `nvs_flash.h`.

### Acknowledgements

- @david-cermak (Espressif) — pointed out the function-pointer-vs-wrap
  issue and the `esp_vfs_register_fd_range` route in
  [espressif/esp-idf#18549](https://github.com/espressif/esp-idf/issues/18549).

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

### IPv6

Tier-1 support: smoltcp's IPv6 protocol is enabled, an IPv6 link-local
address (fe80::/64, modified-EUI-64 derived from the MAC) is generated
and registered automatically when an interface is attached. This means
**ping6 to the link-local works**, NDP and ICMPv6 work, and the
smoltcp-native socket API accepts IPv6 endpoints. Query the address
via `esp_smoltcp_get_ipv6_link_local()`.

What's NOT in v0.1.0:
- SLAAC (Router Solicitation / Router Advertisement processing) —
  smoltcp doesn't ship a client; would need to be hand-rolled
- DHCPv6
- Full BSD-sockets `AF_INET6` support in the `lwip_compat` shim — so
  `esp_http_server` can't yet listen on `::`. Tracked for v0.2.

### Known limits

- Only ESP32-P4 hardware-verified so far. Other RISC-V targets should
  work; classic ESP32 (Xtensa) requires a different Rust target spec.
- ESP-Hosted Wi-Fi path is scaffolded but not yet hardware-verified
  on this repo.
- DNS resolver is single-shot and very minimal (A records only,
  hardcoded primary server). Production users should bring their own.
