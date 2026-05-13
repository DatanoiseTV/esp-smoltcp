# esp_smoltcp

Rust smoltcp networking stack as an ESP-IDF component. Single-task
poll loop, slab-allocated RX frames, smoltcp-native socket API, L2
frame tap, runtime stats.

For drop-in BSD-sockets compatibility with `esp_http_server`,
`esp-tls`, `esp-mqtt` etc, pair this with
[`datanoisetv/esp_smoltcp_lwip_compat`](https://components.espressif.com/components/datanoisetv/esp_smoltcp_lwip_compat).

Full docs and architecture: https://github.com/DatanoiseTV/esp-smoltcp

## Quick use

```c
#include "esp_smoltcp.h"

void app_main(void)
{
    /* Install your own esp_eth driver (PHY, pins, board-specific) */
    esp_eth_handle_t eth = my_install_eth_driver();

    esp_smoltcp_init();
    esp_smoltcp_attach_eth(eth);
    esp_smoltcp_wait_for_ip(ESP_SMOLTCP_IFACE_ETH, 15000);

    /* From here BSD sockets work if esp_smoltcp_lwip_compat is also
     * a dependency; otherwise use the smoltcp-native API in
     * <esp_smoltcp_socket.h>. */
}
```

The complete bring-up example: [examples/eth_basic/](https://github.com/DatanoiseTV/esp-smoltcp/tree/main/examples/eth_basic)

## Hardware-verified performance

Waveshare ESP32-P4-Nano + built-in 100 Mbit/s EMAC: **91.15 Mbit/s
sustained HTTP download**, 0 TX failures, 0 RX drops over 200 MiB.

## Required sdkconfig

```
CONFIG_LWIP_COMPAT_ENABLE=y      # when using the BSD-sockets shim
CONFIG_LWIP_NETIF_LOOPBACK=y     # esp_http_server compile-time check
```

## License

Apache-2.0 OR MIT, your choice.
