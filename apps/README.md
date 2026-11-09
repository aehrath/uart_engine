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

The portable C and Python graphical clients verify every newly opened port
before enabling their sending controls. Each client sends a deterministic
128-byte `TEST` probe and requires the server's CRC-valid completion event with
the same identifier and size. A missing or failed acknowledgement closes the
port after 2.5 seconds and reports that the selected UART is not a BLE UART
server.

The local endpoint makes development possible without two physical serial
adapters. Windows presents it as **Local Simulator** through a built-in named
pipe, so there is no virtual COM driver, port pairing, or administrator setup.
On target hardware, replace the platform register and TX DMA hooks with the MCU
UART/DMA implementation.

## Pure-C client option

`ble_uart_client` provides the Python GUI's core serial features as a pure-C
terminal application. Start `ble_uart_server`, then pass its printed port:

```sh
./build/ble_uart_client /dev/ttys012 4000000
```

It supports command presets, custom opcodes and parameters, raw H4 packets,
automatic CRC framing, continuous receive parsing, and CRC validation. Type
`help` for the command list. Use `mode irq`, `mode dma`, or `mode query` to
control the server receiver and display its authoritative response.

For scripts or smoke tests, provide a preset for a single round trip:

```sh
./build/ble_uart_client /dev/ttys012 4000000 reset
```

## Portable C graphical client

`ble_uart_client_ui` is a pure-C desktop GUI built with raylib and raygui. It
provides editable port and baud fields, HCI presets, custom commands, raw H4
transmission, CRC framing, and a live color-coded traffic view. The same source
builds on macOS, Windows, and Linux. Active entry fields support Cmd+C/X/V on
macOS and Ctrl+C/X/V on Windows and Linux; paste replaces the complete field.
The bulk panel provides the same selector as the Python UI, ranging from 16 KiB
through 16 MiB, alongside the dedicated 128-byte quick test. The **Per-byte
IRQ** checkbox switches between a UART interrupt for every received byte and
the default circular-DMA/IDLE path.

On macOS:

```sh
open "build/BLE UART Client.app"
```

On Linux:

```sh
./build/ble_uart_client_ui
```

On Windows, run `BLE UART Server.exe`, click **Start server**, then run
`ble_uart_client_ui.exe` with **Local Simulator** selected. Enter a device such
as `COM3` only when using physical hardware.

## Graphical server dashboard

On macOS, Linux, and Windows, `ble_uart_server_ui` gives the continuous FreeRTOS server a
matching graphical dashboard. It starts and stops the actual server engine,
displays and copies the generated pseudo-UART path, counts received commands and
transmitted events, and shows the live DMA/HCI log. Its **Per-byte IRQ** checkbox
controls only the server receiver. The **RX IRQ calls** card counts actual RXNE
handler calls in byte mode and completed simulated DMA transactions in DMA
mode. It is local to that server process and is not synchronized to the client
dashboards.

Every graphical dashboard splits the retained traffic log from a real-time RX
pipeline instrument. Blue and cyan traces show the measured bytes per second
crossing the FreeRTOS producer and consumer tasks during each half-second
sample, retaining one minute of history. The Q/R peak gauges report the largest
occupancy still visible in that history, so they agree with visible graph spikes
even after the live buffers have drained.
The footer reports measured enqueue-to-consume latency and CRC/queue/ring drops.
Purple **Q%**, amber **R%**, and gray **D%** graph traces show each sampling
interval's peak queue, ring, and DMA utilization on a normalized 0–100% scale.
Live ring and DMA occupancy return to zero as soon as their bytes are consumed;
the peak traces preserve short bursts for later inspection.
A roughly 255-byte test frame reaches about 24.9% of a 1 KiB ring and nearly
100% of a 256 B ring. Consumer backlog instead fills the 16-slot packet queue,
so heavy competing work primarily drives Q toward 100%.

Headroom is derived from observed backlog growth and currently free queue/ring
capacity. It reads **stable** whenever the consumer is keeping pace. When the
producer is measurably faster, it becomes a time-to-overrun estimate and changes
to amber below ten seconds or red below two seconds. This is deliberately based
on actual task traffic rather than the selected baud's theoretical capacity, so
idle links and successfully drained high-rate links do not produce false
warnings.

The native C client and Python client now carry the same **Clock**, **Baud**,
**Ring**, **DMA**, **Per-byte IRQ**, and **Load** controls as the server. The C client
measures its serial-read-to-parser path; the Python client measures its actual
background-reader-to-Tk-consumer queue. Consequently, their traces and headroom
remain local and can legitimately differ from the server during the same
transfer.

The dashboard also mirrors the client sending controls. It can transmit preset
or custom typed HCI commands, arbitrary raw H4 packets, the 128-byte quick-test
message, and selectable 16 KiB through 16 MiB deterministic ACL transfers to the
connected client. Ordinary packets use the normal FreeRTOS TX queue and DMA
engine. Large transfers retain the same typed 255-byte H4/CRC frames while
coalescing serial writes instead of issuing one host write per frame. Both
directions use the selected baud as the same logical payload clock, so a 2 MiB
transfer at 4 Mbaud completes in about 4.2 seconds whether initiated by a client
or by the server. This prevents pseudo-UART speed from depending on host task
and syscall overhead while preserving responsive UIs.

All three graphical senders advance their progress bars from silent,
baud-derived gauge updates at approximately 30–60 Hz. Human-readable progress
records remain limited to 256 KiB boundaries, so smooth animation does not
consume the retained traffic log or reduce transfer throughput.

`tools/benchmark_bidirectional.py` launches the real server engine and sends the
same deterministic transfer in both directions. It checks CRCs, packet count,
wire-byte count, completion, and timing parity; by default it fails when the
slower direction takes more than 1.15 times as long as the faster direction.
It also accepts `--baud`, `--mode`, `--ring`, and `--dma` for parameter sweeps.

The dashboard send controls are half-duplex. An incoming START frame disables
the command, raw-H4, 128-byte, and large-block send buttons on the receiving C
or Python dashboard; its matching END frame enables them again. The same
buttons remain disabled on the originating app until its own transfer finishes.
Protocol-generated acknowledgements remain available because they do not pass
through these user-facing controls.

Both native dashboards display the repository release version and load the
same platform UI family as the Python client: Avenir Next on macOS, Segoe UI on
Windows, and DejaVu Sans on Linux, with bundled Space Grotesk as a fallback.
Their shared telemetry row reports TX, RX, receive IRQ, and transport state
above consistently styled controls. Every action, dropdown, editable field,
status field, toggle, progress indicator, and log surface uses the shared
rounded geometry and alignment rules. Native windows request high-DPI backing
surfaces and rasterize their fonts at physical display resolution.

The native dashboards share their command and size presets, font management,
dropdown, bounded log, palette, panels, buttons, toggles, status fields, and
clipboard-aware entry implementation through the `apps/common/dashboard_*`
components. The Python client mirrors those same responsibilities in
`python/dashboard_*` modules so visual and behavioral changes have an obvious
counterpart instead of accumulating in one application file.

## KW47 LPUART clock simulation

The server dashboard's **Clock** and **Baud** selectors configure a bit-level
KW47 LPUART simulation. The divisor search covers every legal effective
oversampling ratio from 4 through 32 and every SBR from 1 through 8191, then
selects the pair with the smallest actual baud error. At a 20 MHz peripheral
clock, 4 Mbaud is represented exactly by OSR=5 and SBR=1. The theoretical
5 Mbaud ceiling is represented by OSR=4 and SBR=1, following NXP's documented
`baud = clock / (OSR * SBR)` relationship. The application continues to expose
only the previously established rates through 4 Mbaud.

Pseudo-UART traffic is not rejected merely because `clock / 4` is below the
requested rate. Every packet is instead passed through a continuous 8-N-1
receiver sampler using the selected divisor, asynchronous sampling phase, data
bit centers, and stop-bit sample. The resulting bytes—including corrupt bytes—
continue through DMA, HCI framing, and CRC handling. A yellow `WARNING UART SIM`
line appears only when that sampled packet actually has changed data bits or a
framing error. Large test transfers aggregate failing packet, changed-byte, and
framing-error counts into one completion warning to preserve throughput.

The same simulation is applied in both directions: client-to-server traffic is
sampled by the configured KW47 receiver, while server-to-client traffic uses the
KW47's generated baud against a host receiver configured for the requested
line rate. The selected clock, requested rate, OSR, SBR, actual rate, and signed
ppm error are written to the live server log whenever timing changes. Terminal
users can issue `clock HZ`, `baud RATE`, and `timing` on the server's standard
input. The model follows the baud generator and 4-to-32-sample acquisition range
described in NXP's [KW47 Reference Manual](https://www.nxp.com/docs/en/reference-manual/KW47RM.pdf)
and [LPUART baud-rate application note](https://www.nxp.com/docs/en/application-note/AN14458.pdf).

The **Ring** selector changes the active circular receive capacity from 32 B
through 64 KiB, including the same 32 B, 64 B, 128 B, and 256 B choices offered
by the **DMA** selector. The independent DMA setting changes the maximum bytes
in one receive transaction. Buffer histories in every pipeline graph are
capacity percentages: **Q%** is the message queue, **R%** the circular ring, and
**D%** the DMA transaction. A saturated or overwritten buffer is clamped to the
100% chart boundary instead of drawing outside it.

The server deliberately wakes its producer on UART IDLE, matching a common
embedded DMA design. Consequently, the ring must retain the complete H4 wire
frame that arrived before IDLE; selecting a smaller ring causes a real circular
overwrite and drops that frame. DMA capacity instead controls how many
transfer-complete interrupts occur (approximately `ceil(wire bytes / DMA
size)`) without changing baud-limited throughput. The native C client applies
the same IDLE-delimited frame and DMA-transaction accounting as the server, so
the same frame produces the same ring percentage and IRQ count in either
direction. Its live occupancy returns to zero on the next quiet sample, while
its peak meters retain the largest Q/R percentages in the visible one-minute
history. Both clients drain all
completed DMA transactions already waiting in their serial drivers; ring size
limits an individual IDLE-delimited frame and never acts as a per-render-frame
throughput budget. Native logs retain 256 lines; use the mouse wheel over the
log or click its scrollbar to inspect earlier traffic.

The server dashboard polls live peak occupancy at 10 Hz while retaining graph
samples at 2 Hz. The two horizontal meters are explicitly labeled **Q PEAK**
and **R PEAK**, show one decimal place, and retain the largest values in the
visible one-minute history. When ring and DMA utilization are
equal, the graph draws DMA as a wide gray underlay and ring utilization as an
amber centerline so neither trace can hide the other.

Each graphical app owns its IRQ dashboard counter. The value is cumulative for
that app process. Each ordinary packet resets a separate transmission counter;
the packet's final log entry includes that count once. A chunked test message is
treated as one full transmission, so its counter spans all chunks and appears
only in the completion log entry.

During a client-to-server test in per-byte IRQ mode, the server also publishes
its cumulative and current-transfer interrupt counts at each 256 KiB progress
boundary. The dashboard therefore updates while a large upload is still in
flight, with the final packet count still logged once at completion.

Large test transfers never hex-dump individual ACL chunks. The apps retain
compact start, periodic progress, and completion records; the intentionally
small 128-byte diagnostic message may still be displayed byte-for-byte.

Receive mode and buffer sizes are application-local. Changing a client affects
how that client receives server traffic; changing the server affects how the
server receives client traffic. This keeps every dashboard counter tied to IRQ
calls made by its own process.

On macOS:

```sh
open "build/BLE UART Server.app"
```

The graphical C and Python clients remember their last selected UART port
between launches. They also share saved client settings for clock, baud, ring
capacity, DMA size, large-transfer size, per-byte IRQ mode, and randomized
thread load. Client load changes cross the verified UART as a typed vendor HCI
command and configure the server's real competing FreeRTOS task. The graphical
server saves the same controls independently, so server receive settings do
not overwrite the clients. **Defaults** immediately restores 20 MHz, 4 Mbaud,
16 KiB ring, 256 B DMA, 2 MiB transfer, DMA + IDLE receive mode, and disables
random thread load.

The three load fields are always visible in the main panel: minimum busy
milliseconds, maximum busy milliseconds, and random-range weight. A zero
maximum disables the task.
An empty field normalizes to zero when it loses focus, and legacy saved load
formats migrate to all-zero disabled values.
Tab and Shift+Tab move keyboard focus forward and backward through the fields. Vertical mouse
dragging adjusts min/max in 1 ms steps and R% in one-percent steps. If a new
minimum exceeds the maximum, the maximum follows it upward. If a new maximum
falls below the minimum, the minimum follows it downward. The endpoints are
never swapped, and setting maximum to zero remains the explicit disable operation.
The percentage scales the random offset between minimum and maximum; it is not
an activation probability. The workload
runs above the application consumer but below critical RX producer and TX tasks,
so its measured queue pressure and latency reflect actual FreeRTOS scheduling.
While workload is enabled, local transport ingress rises above it but remains
below the critical producer, ensuring bytes continue entering simulated
UART/DMA while each IDLE event can preempt ingress and drain the ring. With load
disabled, ingress returns to normal application priority to preserve symmetric
wire-rate transfers. Server controls remain above synthetic workload so
overload can always be disabled.
After each busy burst the task leaves a fixed 10 ms idle interval, preventing
long bursts from entering deadline catch-up and permanently starving the
consumer or server-control tasks.
The CLI equivalents are `load status` and `load MIN_MS MAX_MS PERCENT`.

Preferences are stored in the platform user configuration directory:
`~/Library/Application Support/BLE UART/client-port.txt` on macOS,
`%APPDATA%\\BLE UART\\client-port.txt` on Windows, or
`${XDG_CONFIG_HOME:-~/.config}/ble-uart/client-port.txt` on Linux.
The adjacent `client-settings.txt` and `server-settings.txt` files hold the
independent dashboard settings.

On Linux:

```sh
./build/ble_uart_server_ui
```
