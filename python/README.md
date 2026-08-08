# BLE UART HCI Client

This Tk desktop application sends BLE HCI H4 packets over a physical serial port.
Every packet is followed by the same big-endian CRC16-CCITT used by the C UART DMA
stack. Incoming Command, ACL, SCO, Event, and ISO packets are decoded incrementally
and their CRC is checked before display.

## Run

From the repository root:

```sh
python3 -m venv .venv
.venv/bin/python -m pip install -r python/requirements.txt
.venv/bin/python python/ble_uart_app.py
```

The repository's local `.venv` is excluded from version control.

The application is separated into three modules: `ble_uart_app.py` contains the
Tk UI, `ble_protocol.py` contains H4/CRC framing and test-message generation,
and `serial_ports.py` handles port discovery.

Select the UART device and baud rate, connect, and either choose an HCI command
preset or enter a raw H4 packet. The app adds the CRC automatically.

The Python window mirrors the portable C client: both use the same 960x780
four-panel layout, command presets, editable HCI fields, 128-byte quick-test
button, selectable 16 KiB through 16 MiB large transfers, progress display, and
color-coded rolling traffic log. Tk entry fields retain their platform-native
selection and clipboard behavior.

The **Per-byte IRQ** checkbox mirrors the C client and server dashboard. It
selects either one UART RX interrupt per byte or the normal circular-DMA/IDLE
receiver. The server replies with its authoritative state, so changing the
toggle at either end keeps both applications synchronized; connection also
queries the current mode automatically.

## Packet format

```text
[H4 packet type + HCI header + HCI payload] [CRC high byte] [CRC low byte]
```

The receiving firmware must use an IDLE gap, packet boundary, or equivalent
transport framing so that each CRC-protected packet reaches the DMA producer as
one logical frame.

## Test

```sh
.venv/bin/python -m unittest discover -s python/tests -p 'test_*.py'
```
