# esp_smoltcp_glue

Rust [smoltcp](https://github.com/smoltcp-rs/smoltcp) 0.12 packaged as
an ESP-IDF component — `no_std` staticlib for
`riscv32imafc-unknown-none-elf` plus a small C FFI.

You don't usually depend on this directly; depend on
[`datanoisetv/esp_smoltcp`](https://components.espressif.com/components/datanoisetv/esp_smoltcp)
instead, which pulls this in.

Full docs and architecture: https://github.com/DatanoiseTV/esp-smoltcp

## What's in it

- smoltcp 0.12 IPv4 + IPv6 (link-local), DHCPv4 client, ICMP/ICMPv6,
  IGMPv2/MLD multicast, TCP + UDP sockets
- C FFI exposing the smoltcp interface to the rest of the stack
- Static TX scratch pool (16 × 1.5 KiB) to avoid per-packet allocations
- Two-tier allocator: internal SRAM first, PSRAM fallback

## Build requirements

- ESP-IDF v5.4 or later
- Rust nightly with `rust-src` and the `riscv32imafc-unknown-none-elf`
  target. The pinned channel lives in `rust-toolchain.toml` and rustup
  picks it up automatically.

## License

Apache-2.0 OR MIT, your choice.
