# Continuous BLE C UART server

`ble_uart_server` is the continuously running BLE-side companion for the Python
GUI. It uses the real FreeRTOS queues, tasks, notifications, semaphores, circular
RX DMA producer, TX DMA engine, CRC verification, and HCI parser.

Run it first:

```sh
cmake -S . -B build
cmake --build build --parallel
./build/ble_uart_server
```

The server prints a pseudo-UART path such as `/dev/ttys012`. Start the Python app:

```sh
.venv/bin/python python/ble_uart_app.py
```

Type the printed path into the editable **Port** field, click **Connect**, then
send any command preset. The C server runs until Ctrl-C and responds with an HCI
Command Complete event for every valid command.

The pseudo-UART makes local development possible without two physical serial
adapters. On target hardware, replace the platform register and TX DMA hooks with
the MCU UART/DMA implementation.

## Pure-C client option

`ble_uart_client` provides the Python GUI's core serial features as a pure-C
terminal application. Start `ble_uart_server`, then pass its printed port:

```sh
./build/ble_uart_client /dev/ttys012 4000000
```

It supports command presets, custom opcodes and parameters, raw H4 packets,
automatic CRC framing, continuous receive parsing, and CRC validation. Type
`help` for the command list.

For scripts or smoke tests, provide a preset for a single round trip:

```sh
./build/ble_uart_client /dev/ttys012 4000000 reset
```

## Portable C graphical client

`ble_uart_client_ui` is a pure-C desktop GUI built with raylib and raygui. It
provides editable port and baud fields, HCI presets, custom commands, raw H4
transmission, CRC framing, and a live color-coded traffic view. The same source
builds on macOS, Windows, and Linux.

On macOS:

```sh
open "build/BLE UART Client.app"
```

On Linux:

```sh
./build/ble_uart_client_ui
```

On Windows, run `ble_uart_client_ui.exe`. Enter a device such as `COM3`.

## Graphical server dashboard

On macOS and Linux, `ble_uart_server_ui` gives the continuous FreeRTOS server a
matching graphical dashboard. It starts and stops the actual server engine,
displays and copies the generated pseudo-UART path, counts received commands and
transmitted events, and shows the live DMA/HCI log.

On macOS:

```sh
open "build/BLE UART Server.app"
```

On Linux:

```sh
./build/ble_uart_server_ui
```
