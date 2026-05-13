# esp_smoltcp_lwip_compat

BSD-sockets compatibility shim for
[`datanoisetv/esp_smoltcp`](https://components.espressif.com/components/datanoisetv/esp_smoltcp).
Every IDF networking call (`esp_http_server`, `esp-tls`, `esp-mqtt`, …)
talks to smoltcp transparently — zero application source changes.

Full docs and architecture: https://github.com/DatanoiseTV/esp-smoltcp

## How it works

ESP-IDF's `<lwip/sockets.h>` defines `socket()`, `bind()`, etc. as
`static inline` wrappers calling `lwip_socket()`, `lwip_bind()`. We
add `-Wl,--wrap=lwip_socket -Wl,--wrap=lwip_bind ...` linker flags
that redirect every BSD-socket call site to a thin C shim that
proxies to smoltcp. lwIP stays compiled (its headers are needed by
IDF networking source) but its socket layer is unreachable at runtime.

In addition to the wraps:
- `127.0.0.0/8` traffic is handled by an in-RAM loopback table so
  `esp_http_server`'s control-socket pair works without going through
  smoltcp.
- A minimal `esp_netif` shim returns the iface IP, MAC, etc. for the
  IDF components that query that.

## Required sdkconfig

```
CONFIG_LWIP_COMPAT_ENABLE=y
CONFIG_LWIP_NETIF_LOOPBACK=y
```

As of v0.2, the IDF default `CONFIG_VFS_SUPPORT_SELECT=y` is supported —
the shim registers its own VFS for the BSD-socket FD range, overriding
lwIP's claim, so IDF's `select()` dispatches into us cleanly.

## License

Apache-2.0 OR MIT, your choice.
