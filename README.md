# Quectel BC660K-GL NB-IoT Modem Driver for Mbed CE

*Community driver. Not affiliated with or endorsed by Quectel.*

Mbed CE cellular driver for the **Quectel BC660K-GL** NB-IoT module.

## Overview

Operates in **AT sockets (module IP stack)** mode; PPP is not used. Extends
`AT_CellularDevice` with vendor-specific power, configuration, socket
buffering, and diagnostics support.

- 3GPP TS 27.007 / 27.005 AT baseline; vendor `AT+Q...` only where required
  (bands, PSM, TLS, engineering info, PDP defaults).
- Module-IP sockets (`UDPSocket`, `TCPSocket`) via the AT stack.
- Per-socket RX buffering (`QUECTEL_BC660KGL_SocketBuffer`) sized to hold a
  full DTLS flight plus retransmit headroom.
- URCs (`+CEREG`, `+CGACT`, socket events) dispatched via
  `ATHandler::set_urc_handler`; ISRs defer work to the AT queue.

```text
AT_CellularDevice
 └── QUECTEL_BC660KGL
      ├── QUECTEL_BC660KGL_CellularContext     PDP context
      │    └── QUECTEL_BC660KGL_CellularStack  sockets, RX buffering, TLS upload
      ├── QUECTEL_BC660KGL_CellularNetwork     registration, RAT, +CEREG handling
      └── QUECTEL_BC660KGL_CellularInformation IMEI / ICCID / engineering info
```

### Pin connections (Arduino Uno header defaults)

| Function               | Default pin | Notes                          |
|------------------------|-------------|--------------------------------|
| UART TX (host → modem) | D1          | D8 on `NUCLEO_L476RG`          |
| UART RX (modem → host) | D0          | D2 on `NUCLEO_L476RG`          |
| RESET                  | D13         | Pulsed active for 50 ms        |
| PSM-EXIT               | D11         | 1 ms active pulse to wake PSM  |
| BOOT                   | D12         | Forces download mode           |
| RI                     | A0          | Ring indicator input           |
| Polarity               | active high | `1` = active high              |

`vdd-ext-ind` and `net` indicators default to `NC`.

## Integration

CMake target (see [CMakeLists.txt](CMakeLists.txt)):

```cmake
target_link_libraries(<your-app> PRIVATE quectel-bc660kgl)
```

Configure `mbed_app.json5`:

```json
{
    "target_overrides": {
        "NUCLEO_L4A6ZG": {
            "quectel-bc660kgl.provide-default": true,
            "quectel-bc660kgl.bands": "\"8,20\"",
            "nsapi.default-cellular-apn": "\"your.operator.apn\""
        }
    }
}
```

Basic usage:

```cpp
#include "mbed.h"
#include "CellularDevice.h"

int main() {
    CellularDevice *device = CellularDevice::get_default_instance();
    device->init();

    CellularContext *context = device->create_context();
    context->connect();

    UDPSocket socket;
    socket.open(context);
    // ... send/receive ...

    context->disconnect();
}
```

Set `quectel-bc660kgl.provide-default: true` in `mbed_app.json5` to register
the driver as the default `CellularDevice`.

## Configuration

All options live under the `quectel-bc660kgl` namespace in `mbed_app.json5`.
Defaults and the full list are in [mbed_lib.json](mbed_lib.json); the tables
below highlight the keys most commonly overridden.

### Serial

| Option     | Default  | Notes                   |
|------------|----------|-------------------------|
| `tx`       | `null`   | UART TX pin             |
| `rx`       | `null`   | UART RX pin             |
| `rts`      | `null`   | UART RTS pin (optional) |
| `cts`      | `null`   | UART CTS pin (optional) |
| `baudrate` | `115200` | Serial baud rate        |

### Control pins

| Option        | Default | Notes                                                  |
|---------------|---------|--------------------------------------------------------|
| `reset`       | `null`  | Reset pin, pulsed active for 50 ms                     |
| `boot`        | `null`  | Boot pin, forces download mode                         |
| `psm-exit`    | `null`  | Wakes module from Deep/Light/Sleep (1 ms active pulse) |
| `vdd-ext-ind` | `null`  | Input; monitors `VDD_EXT` to detect deep-sleep         |
| `net`         | `null`  | NET indicator input                                    |
| `ri`          | `null`  | Ring indicator input                                   |
| `polarity`    | `null`  | `1` = active high, `0` = active low                    |

### Radio and IP stack

| Option                      | Default  | Notes                                                                              |
|-----------------------------|----------|------------------------------------------------------------------------------------|
| `provide-default`           | `false`  | Register as the default `CellularDevice`                                           |
| `configure`                 | `false`  | Run one-shot modem configuration + reboot on init (~50 s)                          |
| `bands`                     | `"8,20"` | Comma-separated NB-IoT bands to enable                                             |
| `default-pdp-type`          | `null`   | PDP type for CID 0: `IP`, `IPV6`, or `IPV4V6` (many NB-IoT networks are IPv4 only) |
| `socket-count`              | `5`      | Maximum concurrent sockets                                                         |
| `socket-buffer-chunk-size`  | `255`    | Max payload bytes per RX chunk (`uint8_t`, max 255)                                |
| `socket-buffer-queue-depth` | `24`     | Chunk slots per socket; must hold one full DTLS flight plus headroom               |

### Diagnostics

| Option                      | Default | Notes                                                                    |
|-----------------------------|---------|--------------------------------------------------------------------------|
| `rssi`                      | `false` | Enable RSSI data provider                                                |
| `rssi-history`              | `false` | Enable RSSI history                                                      |
| `get-op`                    | `false` | Include operator info in data provider                                   |
| `get-engineering-info`      | `false` | Enable `get_engineering_info()` (PSRP/RSRQ/RSSI/SINR)                    |
| `engineering-rssi-sinr-add` | `false` | Append RSSI + SINR to engineering info (requires `get-engineering-info`) |

## Layout

```text
bc660kgl/
├── CMakeLists.txt
├── mbed_lib.json
├── README.md
├── include/bc660kgl/
│   ├── QUECTEL_BC660KGL.h
│   ├── QUECTEL_BC660KGL_CellularContext.h
│   ├── QUECTEL_BC660KGL_CellularNetwork.h
│   ├── QUECTEL_BC660KGL_CellularInformation.h
│   ├── QUECTEL_BC660KGL_CellularStack.h
│   └── QUECTEL_BC660KGL_SocketBuffer.h
└── src/                       # matching .cpp files
```

## License

Apache License 2.0. See `LICENSE`.
