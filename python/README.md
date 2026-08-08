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

Connecting first performs the same identity check as the portable C UI: the
client sends a 128-byte deterministic probe and waits up to 2.5 seconds for the
server's matching, CRC-valid acknowledgement. Until that succeeds the dashboard
shows **VERIFYING** and all transmit controls remain disabled. A wrong or silent
UART is closed automatically and an error remains visible in the status bar.

The Python window mirrors the portable C client: both use the same 1120x960
four-panel layout, command presets, editable HCI fields, 128-byte quick-test
button, selectable 16 KiB through 16 MiB large transfers, progress display, and
color-coded rolling traffic log. Tk entry fields retain their platform-native
selection and clipboard behavior.

The title and heading display the shared repository release version. The UI
uses Segoe UI, Avenir Next, or DejaVu Sans according to the host platform, with
the same dark telemetry cards and rounded high-contrast action controls as the
native dashboards.

The **Per-byte IRQ** checkbox mirrors the C client and server dashboard. It
selects either one UART RX interrupt per byte or the normal circular-DMA/IDLE
receiver. The server replies with its authoritative state, so changing the
toggle at either end keeps both applications synchronized; connection also
queries the current mode automatically.

The IRQ dashboard count is local to this Python process and accumulates while
incoming serial bytes are consumed through the selected per-byte receive path.
Each received packet resets its transmission count. Multi-chunk test data is
treated as one full transmission, with one aggregate IRQ count written to the
log only after the final chunk.

Transfers larger than 128 bytes use compact start/progress/completion log lines
instead of formatting every ACL chunk as hexadecimal, avoiding UI logging as a
throughput bottleneck.

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
