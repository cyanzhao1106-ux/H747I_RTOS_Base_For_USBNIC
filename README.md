# RTL815x USB-NIC CDC Host — STM32H747I-DISCO Example

A worked example of driving a **Realtek RTL815x USB Ethernet dongle** as a
**USB Host** on an STM32H7, bridged to **lwIP** over FreeRTOS. The dongle's
**CDC-ECM** or **CDC-NCM** transport is selectable at build time. A small
MIPI-DSI dashboard shows the live NIC status.

> Target board: **STM32H747I-DISCO** (dual-core Cortex-M7 + M4). The application
> runs entirely on the **CM7** core; the CM4 core is parked in D2 STOP.

---

## Features

- USB Host class driver for Realtek RTL815x family NICs (`CM7/USBNIC/`).
- Build-time selection between **CDC-ECM** and **CDC-NCM** transports.
- lwIP integration with DHCP client; raw-Ethernet TX/RX bridged to a single
  `netif`.
- Throughput-oriented data path: dual bulk-IN **ping-pong** RX buffers and a
  lock-free single-producer/consumer **TX ring**, so the TCP/IP thread is never
  blocked on USB latency.
- Built-in **iperf** TCP server (port **5001**) for throughput testing.
- MIPI-DSI **status dashboard** (display only): chip, VID:PID, MAC, link
  state/speed, MTU, assigned IPv4, and the active CDC mode.
- UART debug log on the ST-LINK VCP (**USART1 @ 115200 8N1**).

---

## Supported chips

All Realtek, USB Vendor ID **`0x0BDA`**:

| Product ID | Chip       | Notes                                   |
|-----------:|------------|-----------------------------------------|
| `0x8152`   | RTL8152B   | USB 2.0, 10/100 Mbps                    |
| `0x8153`   | RTL8153    | USB 3.0, 10/100/1000 Mbps               |
| `0x8156`   | RTL8156B   | USB 3.x, 2.5 GbE                        |
| `0x8157`   | RTL8157     | USB 3.x, 5 GbE                          |
| `0x815A`   | RTL8159    | multi-gig                               |

Chip/VID/PID matching lives in `CM7/USBNIC/usbh_rtknic.h`.

> Note: the CDC `CONNECTION_SPEED_CHANGE` bitrate fields are 32-bit and saturate
> at ~4.29 Gbps, so 5G/10G-capable parts may report `0xFFFFFFFF` for the rate.

---

## Software components & versions

| Component                  | Version            |
|----------------------------|--------------------|
| MCU / Board                | STM32H747XI / STM32H747I-DISCO |
| Toolchain                  | Keil MDK-ARM, Arm Compiler **6.24** |
| STM32H7xx HAL Driver       | **v1.11.6**        |
| CMSIS Core (Cortex-M)      | **5.6.0**, CMSIS-RTOS2 API |
| FreeRTOS kernel            | **V10.6.2**        |
| lwIP                       | **2.2.1**          |
| USB Host Library           | ST STM32 USB Host Library (Core + custom RTKNIC class) |

---

## Repository layout

```
H747I_RTOS_Base_For_USBNIC/
├── CM7/
│   ├── Core/          # main.c, FreeRTOS config, clocks, LCD dashboard (lcd_app.c)
│   ├── USBNIC/        # RTL815x USB host class + ECM/NCM protocol modules
│   │   ├── usbh_rtknic.c/.h        # class core, RX ping-pong, TX ring, accessors
│   │   ├── usbh_rtknic_ecm.c       # CDC-ECM transport
│   │   ├── usbh_rtknic_ncm.c       # CDC-NCM (NTB) transport
│   │   └── usbh_rtknic_reg.c       # PLA/USB register access over EP0 vendor control
│   ├── LWIP/          # lwipopts.h + usbnic_netif.c (netif glue, iperf)
│   └── USB_HOST/      # ST USB host application glue
├── CM4/               # second core (parked)
├── Drivers/           # CMSIS + STM32H7 HAL + BSP
├── Middlewares/       # FreeRTOS, lwIP, ST USB Host Library
└── MDK-ARM/           # Keil project (.uvprojx)
```

---

## Build

1. Open `H747I_RTOS_Base_For_USBNIC/MDK-ARM/H747I_RTOS_Base_For_USBNIC.uvprojx`
   in Keil MDK (Arm Compiler 6).
2. Select the **`H747I_RTOS_Base_For_USBNIC_CM7`** target.
3. Build (F7) and flash (F8).

Headless build from a shell:

```sh
"C:/Keil_MDK-ARM/UV4/UV4.exe" -b H747I_RTOS_Base_For_USBNIC/MDK-ARM/H747I_RTOS_Base_For_USBNIC.uvprojx \
    -t H747I_RTOS_Base_For_USBNIC_CM7 -j0 -o build_log.txt
```

(`UV4` exit code: `0` = OK, `1` = warnings, `>=2` = errors.)

---

## Switching CDC-ECM ⇄ CDC-NCM

The transport is chosen by the **`RTKNIC_USE_NCM`** preprocessor define:

- **Defined**  → CDC-NCM (NTB framing) — `RTKNIC_NCM_Ops`
- **Not defined** → CDC-ECM (one frame per transfer) — `RTKNIC_ECM_Ops`

(See `CM7/USBNIC/usbh_rtknic.c`, build-time `RTKNIC_ACTIVE_OPS` selection.)

**In Keil:** Project → Options for Target `..._CM7` → **C/C++** tab → *Define*.

- For **NCM**: include `RTKNIC_USE_NCM`
  ```
  CORE_CM7,USE_HAL_DRIVER,STM32H747xx,RTKNIC_USE_NCM
  ```
- For **ECM**: remove `RTKNIC_USE_NCM`
  ```
  CORE_CM7,USE_HAL_DRIVER,STM32H747xx
  ```

The selected mode is printed on the boot banner (UART) and shown on the LCD
dashboard. Rebuild after changing the define.

---

## Network / testing

- The NIC obtains an address via **DHCP**; the assigned IP/GW/mask are logged on
  USART1 and shown on the LCD.
- An **iperf TCP server** listens on port **5001** (bound to `ANY`, survives DHCP
  renewals). Test from a host on the same subnet, e.g.:
  ```sh
  iperf -c <board-ip> -p 5001 -i 1
  ```

---

## LCD status dashboard

A low-priority FreeRTOS task renders a display-only dashboard on the MIPI-DSI
panel (framebuffer in external SDRAM): title bar with the active CDC mode, then
**Device / VID:PID / MAC / Link / MTU / IP**. Implementation in
`CM7/Core/Src/lcd_app.c`; the read-only NIC snapshot API is
`usbnic_get_info()` in `CM7/LWIP/usbnic_netif.c`.

---

## Licensing

This example bundles third-party components under their own licenses:

- **STM32 HAL / BSP / CMSIS / USB Host Library** — © STMicroelectronics
  (see `Drivers/.../*_readme.txt`, `LICENSE.txt`).
- **FreeRTOS** — MIT (`Middlewares/Third_Party/FreeRTOS/.../readme.txt`).
- **lwIP** — BSD-style (`Middlewares/Third_Party/LwIP/...`).

The RTL815x USB host class (`CM7/USBNIC/`) and lwIP glue (`CM7/LWIP/`) are the
example-specific code in this repository.
