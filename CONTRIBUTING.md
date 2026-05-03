# Contributing

## What's testable today

- ESP32-P4 with built-in EMAC (verified on Waveshare P4-Nano, ~91 Mbit/s)
- ESP-IDF v5.5 and v6.0
- Rust nightly (pinned in `components/esp_smoltcp_glue/rust-toolchain.toml`)

## What we'd love help with

- Hardware verification on **non-P4 RISC-V targets** (ESP32-C5, C6, P5
  when available) — needs a custom Rust target spec for the right ABI
- **ESP-Hosted Wi-Fi** path (C5/C6 co-processor over SDIO/SPI) is wired
  but never tested on real silicon
- Better DNS resolver: caching, retry across multiple servers, AAAA
- mDNS responder that doesn't need lwIP's `netif_list`
- IPv6 (smoltcp supports it; we'd just need to wire it through)

## Build the example

```bash
git clone <this repo>
cd esp-smoltcp/examples/eth_basic

# One-time: install Rust nightly + riscv32imafc target
rustup toolchain install nightly --profile minimal
rustup target add riscv32imafc-unknown-none-elf --toolchain nightly

source $IDF_PATH/export.sh                       # ESP-IDF v5.5 or v6.0
idf.py set-target esp32p4
idf.py build flash monitor
```

## Component-only build (no example)

This is what users get when they `idf.py add-dependency`:

```bash
mkdir my_app && cd my_app
# Use your own CMakeLists.txt + main/main.c.
echo 'dependencies:' > main/idf_component.yml
echo '  datanoisetv/esp_smoltcp: "*"' >> main/idf_component.yml
echo '  datanoisetv/esp_smoltcp_lwip_compat: "*"' >> main/idf_component.yml
idf.py reconfigure
```

## Architecture rules of thumb

- **smoltcp is single-threaded.** All calls go through `g_smoltcp_lock`.
  The poll task takes the lock around `iface.poll()`; the socket-API
  wrappers take it around their per-call smoltcp ops. Never touch
  smoltcp without the lock.
- **The Rust side never holds the lock.** It runs under it because the
  C-side caller acquired it before invoking the FFI.
- **Hot-path C functions go in IRAM.** `eth_input_cb`, `eth_tx`,
  `net_frame_alloc/free`, `net_stack_post_rx`, `smoltcp_glue_tx_cb`,
  `smoltcp_glue_now_us` all carry `IRAM_ATTR`. Each call without it
  is an icache miss at ~5 K calls/sec under load.
- **Apps own their L2 driver.** The component does not configure pins,
  PHYs, or transports. The app builds an `esp_eth_handle_t` (or in the
  Wi-Fi case, an `esp_remote_channel_t`) and hands it to
  `esp_smoltcp_attach_*()`.
- **Don't add board-specific Kconfig.** Pin maps, PHY models, and SDIO
  timings belong to the application's `sdkconfig.defaults.<board>`.

## Performance tuning rules learned the hard way

These are documented in `examples/eth_basic/sdkconfig.defaults` so
copies stay accurate:

- `CONFIG_VFS_SUPPORT_SELECT=n` is **mandatory** with this stack.
  Otherwise `select()` goes through ESP-IDF's VFS layer which queries
  lwIP's socket table — and our smoltcp sockets aren't in that table,
  so select always reports "not ready" and `esp_http_server` stalls.
- **Don't enable `CONFIG_COMPILER_OPTIMIZATION_PERF` (`-O2`).** It
  regresses throughput from 86 to 28 Mbit/s. Some IDF data-plane code
  is correctness-tuned for `-Og`. Stay at the default.
- **CPU 400 MHz is unstable** on the P4-Nano default PLL config —
  asserts in `esp_clk_init` at boot. 360 MHz is fine.
- **Allocator is internal-RAM-first**. Don't override
  `MALLOC_CAP_INTERNAL` preference in `alloc_shim.rs` — PSRAM-backed
  ring buffers tank throughput by ~10×.

## Licensing

Apache-2.0 OR MIT — pick whichever fits your downstream. Contributions
are dual-licensed under the same terms.
