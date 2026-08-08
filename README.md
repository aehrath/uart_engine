# UART DMA / BLE HCI demo

This project is split into small, reusable components and built entirely with
CMake.

## Layout

```text
include/uart_dma/       Public embedded-library headers
src/                    FreeRTOS UART/DMA, ring buffer, CRC, and HCI parser
apps/common/            Cross-platform serial-port and H4 wire helpers
apps/server/            Pseudo-UART and server-side serial ingress
apps/ble_uart_*.c       C command-line and graphical app entry points
python/ble_protocol.py  Python H4 framing, CRC, parser, and test messages
python/serial_ports.py  Python serial-port discovery
python/ble_uart_app.py  Python graphical app entry point
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

All graphical apps expose the same synchronized **Per-byte IRQ** toggle. The
default receive path uses circular DMA plus UART IDLE notification; the optional
path stores each byte from RXNE in the same ring buffer. A typed HCI vendor
command/event exchange keeps the server dashboard and connected client state in
sync.

The graphical server can also originate the same HCI, raw H4, 128-byte, and
selectable large test traffic as the clients. Its live metrics include the
actual number of receive-byte IRQ handler invocations.

Application releases are numbered from the repository [`VERSION`](VERSION)
file. Every dashboard displays that version in its window title and heading.
The native dashboards bundle Noto Sans for consistent antialiased text, while
the Python dashboard selects the corresponding native sans-serif UI family.

IRQ counters are deliberately application-local rather than synchronized over
HCI. Dashboards show a cumulative process-lifetime value, while logs show a
fresh per-packet value once an ordinary packet—or an entire chunked test
transmission—has completed.
