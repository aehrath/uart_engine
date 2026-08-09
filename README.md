# UART DMA / BLE HCI demo

This project is split into small, reusable components and built entirely with
CMake.

## Layout

```text
include/uart_dma/       Public embedded-library headers
src/                    FreeRTOS UART/DMA, ring buffer, CRC, and HCI parser
apps/common/            Shared protocol, serial, LPUART timing, logging, and UI code
apps/server/            Pseudo-UART and server-side serial ingress
apps/ble_uart_*.c       C command-line and graphical app entry points
python/dashboard_*.py   Python mirrors of the native dashboard components
python/ble_protocol.py  Python H4 framing, CRC, parser, and test messages
python/serial_ports.py  Python serial-port discovery
python/ble_uart_app.py  Python graphical controller and layout entry point
demo/                   Embedded round-trip demo and macOS wrapper app
tests/                  Native integration tests
```

The public [`uart_dma/uart_dma.h`](include/uart_dma/uart_dma.h) header is a
convenience umbrella. Individual components can instead include only
`uart_crc16.h`, `uart_dma_ring_buffer.h`, `ble_hci_parser.h`, `ble_hci_wire.h`,
`uart_dma_messages.h`, or `uart_dma_platform.h`.

## Architecture

See the [runtime dataflow graph](docs/architecture.md#runtime-dataflow) and
[repository component graph](docs/architecture.md#repository-component-graph)
for the complete relationship between the clients, pseudo-UART server,
FreeRTOS/DMA pipeline, libraries, demos, tests, and documentation targets.

## Build and test

```sh
cmake -S . -B build
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

## API documentation

All project-owned C, Objective-C, and Python modules use Doxygen comments.
Doxygen is a default build dependency and the normal build generates the HTML
documentation after compiling the enabled native targets:

```sh
brew install doxygen                    # macOS
cmake -S . -B build
cmake --build build --parallel          # also builds the docs target
open build/docs/html/index.html       # macOS
```

The configuration treats documentation warnings as errors. Generated output
lives under `build/docs` and is not stored with the source. Distribution or
cross-compilation builds may explicitly omit documentation with
`-DUART_DMA_BUILD_DOCS=OFF`.

See [`apps/README.md`](apps/README.md) for the C server and clients, and
[`python/README.md`](python/README.md) for the Python client.

All graphical apps expose an application-local **Per-byte IRQ** toggle. The
default path counts one receiver IRQ for each completed DMA/read transaction;
the optional path counts one RXNE receiver IRQ for every received wire byte.
Clock, baud, ring capacity, DMA size, large-transfer size, and IRQ mode are
restored across launches. The C and Python clients share client preferences,
the server keeps an independent settings file, and every UI provides a
**Defaults** button that immediately reapplies the common baseline settings.
Ring-buffer capacity and DMA transaction size are independently selectable in
each dashboard. Ring choices run from 32 B through 64 KiB; DMA choices run from
32 B through 4 KiB.

The graphical server can also originate the same HCI, raw H4, 128-byte, and
selectable large test traffic as the clients. Every graphical app's live metrics include the
actual number of receive-byte IRQ handler invocations. A rolling pipeline
chart compares measured FreeRTOS producer and consumer byte rates over the last
minute. Queue and circular-ring gauges, enqueue-to-consume latency, drop counts,
and measured time-to-overrun make the remaining receive headroom visible while
clock, baud, DMA, ring, and IRQ-mode parameters are changed. The native C and
Python clients expose the same parameter row and monitor as the server while
retaining application-local measurements.
Queue (**Q%**), ring (**R%**), and DMA (**D%**) histories use their own selected
capacities as 100%, so differently sized buffers remain directly comparable and
an overflow cannot escape the chart bounds.

Application releases are numbered from the repository [`VERSION`](VERSION)
file. Every dashboard displays that version in its window title and heading.
The native and Python dashboards select the same platform UI family: Avenir
Next on macOS, Segoe UI on Windows, and DejaVu Sans on Linux. Native builds
retain bundled Space Grotesk as a fallback. All dashboards use matching bright,
8-pixel-radius buttons and fields against a layered dark instrument-panel
layout; telemetry cards retain their roomier 13-pixel corners. Editable text is
left-aligned, while button and dropdown values are centered. Native dashboards
use high-DPI backing surfaces and DPI-scaled font atlases on Retina/HiDPI
displays.

IRQ counters are deliberately application-local rather than synchronized over
HCI. Dashboards show a cumulative process-lifetime value, while logs show a
fresh per-packet value once an ordinary packet—or an entire chunked test
transmission—has completed. Each graphical log retains 256 lines and supports
mouse-wheel or scrollbar navigation without forcing a user back to the tail.
