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

The application uses the same component boundaries as the C dashboards.
`dashboard_config.py`, `dashboard_font.py`, `dashboard_dropdown.py`,
`dashboard_log.py`, `dashboard_pipeline.py`, and `dashboard_ui.py` mirror the corresponding files in
`apps/common`. `lpuart_sim.py` mirrors the native KW47 OSR/SBR divisor search
and deterministic 8-N-1 sampler. `ble_protocol.py` contains H4/CRC framing and test-message
generation, `serial_ports.py` handles port discovery, and `ble_uart_app.py`
contains only the application controller and concrete client layout.

Select the UART device and baud rate, connect, and either choose an HCI command
preset or enter a raw H4 packet. The app adds the CRC automatically.

Connecting first performs the same identity check as the portable C UI: the
client sends a 128-byte deterministic probe and waits up to 2.5 seconds for the
server's matching, CRC-valid acknowledgement. Until that succeeds the dashboard
shows **VERIFYING** and all transmit controls remain disabled. A wrong or silent
UART is closed automatically and an error remains visible in the status bar.
Serial-device opening also runs outside Tk's event thread, so an invalid or
unresponsive driver can be cancelled and cannot freeze the window.

The Python window mirrors the portable C client: both use the same 1120x960
four-panel layout, command presets, editable HCI fields, 128-byte quick-test
button, selectable 16 KiB through 16 MiB large transfers, progress display, and
a color-coded 256-line scrollable traffic log. Custom rounded entry fields
retain Tk's native selection and clipboard behavior.

The title and heading display the shared repository release version. The UI
uses Segoe UI, Avenir Next, or DejaVu Sans according to the host platform, with
the same dark telemetry cards, 8-pixel control corners, centered dropdown and
button text, and left-aligned editable fields as the native dashboards.

The selected UART port is restored on the next launch. The Python and C clients
use the same `client-port.txt` location in the platform user configuration
directory, and save typed, dropdown-selected, and attempted port names.
They also share `client-settings.txt`, which restores clock, baud, ring, DMA,
large-block size, and per-byte IRQ mode. The **Defaults** button applies and
saves the common 20 MHz, 4 Mbaud, 16 KiB ring, 256 B DMA, 2 MiB block, and
DMA + IDLE defaults. The C server keeps its equivalent controls independently
in `server-settings.txt`.

The **Clock**, **Baud**, **Ring**, **DMA**, and **Per-byte IRQ** controls match
the native server and C client. The toggle controls this Python process only.
Byte mode counts one receiver IRQ per received wire byte. DMA mode counts
`ceil(frame bytes / DMA size)` transaction completions. The adjacent **Ring**
and **DMA** selectors set the IDLE-delimited frame capacity and maximum DMA
transaction size, respectively.
The ring menu includes 32 B, 64 B, 128 B, and 256 B alongside its larger sizes,
matching the small DMA choices.
The background reader continuously drains configured DMA-sized transactions;
ring capacity validates each completed H4 frame and does not throttle bytes to
one ring-capacity span per Tk refresh.

All four send buttons are half-duplex controls. They disable while this client
is sending a test transfer and from each incoming test START through its END,
matching both native dashboards and preventing competing user-started sends.

The lower monitor applies the same pipeline units as the FreeRTOS server: a
16-slot completed-packet queue, H4 wire bytes in the circular ring, and bytes in
one DMA transaction. It retains one minute of half-second rate samples, reports
enqueue-to-consume latency and CRC/queue/ring drops, and derives time-to-overrun
only when the measured producer rate exceeds the consumer rate.
The graph also retains queue (**Q%**), ring (**R%**), and DMA (**D%**) peak
utilization as percentages of their selected capacities, clamped at 100%.
Short ring and DMA peaks remain visible for one half-second display interval,
then clear on the next quiet sample.
The monitor repaints at 20 Hz, labels its horizontal meters **Q PEAK** and
**R PEAK**, and shows one-decimal percentages. Overlapping DMA and ring graph
traces remain separately visible through a wide-gray/amber-centerline style.

The IRQ dashboard count is local to this Python process and accumulates while
incoming serial bytes are consumed through the selected receive path.
Each received packet resets its transmission count. Multi-chunk test data is
treated as one full transmission, with one aggregate IRQ count written to the
log only after the final chunk.

Transfers larger than 128 bytes use compact start/progress/completion log lines
instead of formatting every ACL chunk as hexadecimal, avoiding UI logging as a
throughput bottleneck. Separate gauge-only notifications advance the progress
bar at approximately 30–60 Hz, matching the native C client and server without
adding those animation updates to the log.

The receive parser uses the platform-optimized CRC16 primitive and compacts a
coalesced serial buffer only once per read. Server-to-client transfers therefore
remain fast even when one read contains thousands of framed test packets.

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
