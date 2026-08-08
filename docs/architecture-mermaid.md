# Architecture

This document provides two complementary views of the repository. The runtime
dataflow follows bytes through a complete client/server exchange. The component
graph shows source ownership, dependencies, build targets, tests, and generated
documentation.

Solid arrows represent runtime flow or direct code dependencies. Dashed arrows
represent CMake target and build dependencies.

## Runtime dataflow

```mermaid
flowchart TB
    User["User<br/>commands, raw H4, 128 B / 2 MB tests"]

    subgraph Clients["Client options"]
        CCLI["C CLI<br/>ble_uart_client"]
        CGUI["Portable C GUI<br/>ble_uart_client_ui<br/>raylib + raygui"]
        PYGUI["Python GUI<br/>ble_uart_app.py<br/>Tk + pyserial"]
    end

    subgraph ClientProtocol["Client-side protocol"]
        CH4["C H4 helpers<br/>typed commands, CRC16, stream parser"]
        CSERIAL["C serial abstraction<br/>Windows / macOS / Linux"]
        PYPROTO["Python protocol<br/>H4 framing, CRC16, test chunks"]
        PYPORTS["Python port discovery"]
    end

    User --> CCLI
    User --> CGUI
    User --> PYGUI
    CCLI --> CH4
    CGUI --> CH4
    CH4 --> CSERIAL
    PYGUI --> PYPROTO
    PYGUI --> PYPORTS
    PYPROTO --> PYSERIAL["pyserial"]
    PYPORTS --> PYSERIAL
    CSERIAL --> UART["Physical UART<br/>or pseudo-terminal slave"]
    PYSERIAL --> UART

    subgraph Server["Continuous BLE C server"]
        PTY["Pseudo-UART master"]
        Reader["Serial_RX FreeRTOS task"]
        Ingress["H4 serial ingress<br/>frame reassembly"]
        Inject["Simulated DMA injection<br/>circular RX buffer + NDTR"]
        IdleIRQ["UART IDLE ISR"]
    end

    UART <--> PTY
    PTY --> Reader
    Reader --> Ingress
    Ingress --> Inject
    Inject --> IdleIRQ

    subgraph RxPipeline["FreeRTOS RX pipeline"]
        Notify["Producer notification"]
        Producer["Producer task"]
        Ring["Circular DMA ring reader"]
        RxCRC["CRC16 validation<br/>strip trailer"]
        RxQueue["RX PacketMessage queue"]
        Consumer["Consumer task"]
        Parser["Incremental typed HCI parser"]
    end

    IdleIRQ --> Notify
    Notify --> Producer
    Producer --> Ring
    Ring --> RxCRC
    RxCRC -->|"valid frame"| RxQueue
    RxCRC -->|"bad CRC / overflow"| Drop["Drop frame"]
    RxQueue --> Consumer
    Consumer -->|"one byte at a time"| Parser

    subgraph Handlers["Server HCI handlers"]
        Command["Command handler<br/>print received parameters"]
        ACL["ACL handler<br/>ordinary data or TEST chunks"]
        Verify["Transfer verifier<br/>offset, ID, size, pattern"]
        Response["Typed response<br/>Command Complete or TEST event"]
    end

    Parser -->|"HCI Command"| Command
    Parser -->|"HCI ACL"| ACL
    ACL --> Verify
    Command --> Response
    Verify -->|"end of transfer"| Response

    subgraph TxPipeline["FreeRTOS TX pipeline"]
        TxAPI["UartDma_SendPacket"]
        TxQueue["TX PacketMessage queue"]
        TxTask["TX engine task"]
        Semaphore["TX DMA semaphore"]
        TxCRC["Append CRC16 trailer"]
        TxHook["Platform TX DMA hook"]
        TxIRQ["DMA completion ISR"]
    end

    Response --> TxAPI
    TxAPI --> TxQueue
    TxQueue --> TxTask
    Semaphore --> TxTask
    TxTask --> TxCRC
    TxCRC --> TxHook
    TxHook --> PTY
    TxHook --> TxIRQ
    TxIRQ --> Semaphore
    UART --> CH4
    UART --> PYPROTO
    CH4 -->|"validated response"| CCLI
    CH4 -->|"validated response"| CGUI
    PYPROTO -->|"validated response"| PYGUI
```

## Repository component graph

```mermaid
%%{init: {"flowchart": {"defaultRenderer": "elk"}}}%%
flowchart TB
    subgraph Build["Build and project configuration"]
        CMake["CMakeLists.txt"]
        Config["config/FreeRTOSConfig.h"]
        Ignore[".gitignore"]
        Readmes["README.md<br/>apps/README.md<br/>python/README.md"]
    end

    subgraph External["Fetched or system dependencies"]
        FreeRTOS["FreeRTOS-Kernel 11.3.0"]
        Raylib["raylib 6.0"]
        Raygui["raygui 4.0"]
        PySerial["pyserial"]
        Tk["Tk"]
        Doxygen["Doxygen"]
        Cocoa["macOS Cocoa"]
        PTYAPI["POSIX PTY / termios"]
    end

    CMake -.-> FreeRTOS
    CMake -.-> Raylib
    CMake -.-> Raygui
    CMake -.-> Doxygen
    Config --> FreeRTOS

    subgraph Wire["Typed HCI wire protocol — ble_hci_wire"]
        WireH["include/uart_dma/ble_hci_wire.h<br/>packed Command, ACL, SCO, Event, ISO types"]
        WireC["src/ble_hci_wire.c<br/>typed header inspection"]
        WireH --> WireC
    end

    subgraph Core["FreeRTOS UART/DMA library — uart_dma"]
        Umbrella["include/uart_dma/uart_dma.h<br/>public umbrella API"]
        Messages["uart_dma_messages.h<br/>PacketMessage"]
        Platform["uart_dma_platform.h<br/>UART/DMA register abstraction"]
        RingH["uart_dma_ring_buffer.h"]
        RingC["uart_dma_ring_buffer.c"]
        CrcH["uart_crc16.h"]
        CrcC["uart_crc16.c"]
        ParserH["ble_hci_parser.h"]
        ParserC["ble_hci_parser.c"]
        Engine["uart_dma_freertos.c<br/>ISRs, queues, producer, consumer, TX engine"]

        RingH --> RingC
        CrcH --> CrcC
        ParserH --> ParserC
        WireH --> ParserC
        RingC --> Engine
        CrcC --> Engine
        ParserC --> Engine
        Messages --> Engine
        Platform --> Engine
        Umbrella --> Messages
        Umbrella --> Platform
        Umbrella --> RingH
        Umbrella --> CrcH
        Umbrella --> ParserH
        Umbrella --> WireH
        FreeRTOS --> Engine
    end

    CMake -.-> Wire
    CMake -.-> Core

    subgraph Common["Native app support — ble_uart_app_common"]
        H4["apps/common/h4_wire.h + .c<br/>commands, CRC, hex, stream parser"]
        Serial["apps/common/serial_port.h + .c<br/>Win32 / POSIX serial"]
        TestProto["apps/common/test_transfer_protocol.h<br/>typed deterministic chunks"]
        WireH --> H4
        CrcC --> H4
        PTYAPI --> Serial
    end

    CMake -.-> Common

    subgraph NativeClients["Native client applications"]
        ClientCLI["apps/ble_uart_client.c<br/>ble_uart_client"]
        ClientUI["apps/ble_uart_client_ui.c<br/>ble_uart_client_ui"]
        ClientCLI --> H4
        ClientCLI --> Serial
        ClientCLI --> TestProto
        ClientUI --> H4
        ClientUI --> Serial
        ClientUI --> TestProto
        Raylib --> ClientUI
        Raygui --> ClientUI
    end

    CMake -.-> ClientCLI
    CMake -.-> ClientUI

    subgraph NativeServer["Continuous server applications"]
        Pseudo["apps/server/pseudo_uart.h + .c"]
        Ingress["apps/server/serial_ingress.h + .c"]
        Server["apps/ble_uart_server.c<br/>ble_uart_server engine"]
        ServerUI["apps/ble_uart_server_ui.c<br/>ble_uart_server_ui dashboard"]
        WireH --> Ingress
        Pseudo --> Server
        Ingress --> Server
        TestProto --> Server
        Engine --> Server
        ServerUI -->|"process control + logs"| Server
        Raylib --> ServerUI
        Raygui --> ServerUI
        PTYAPI --> Pseudo
    end

    CMake -.-> Server
    CMake -.-> ServerUI

    subgraph PythonApp["Python client"]
        PyProtocol["python/ble_protocol.py<br/>CRC, H4 parser, test generators"]
        PyPorts["python/serial_ports.py"]
        PyApp["python/ble_uart_app.py<br/>Tk GUI + worker threads"]
        Requirements["python/requirements.txt"]
        PyProtocol --> PyApp
        PyPorts --> PyApp
        Requirements --> PySerial
        PySerial --> PyApp
        Tk --> PyApp
    end

    subgraph Demo["Round-trip demonstration"]
        DemoCLI["demo/uart_dma_demo.c<br/>FreeRTOS app + simulated controller"]
        DemoApp["demo/uart_dma_demo_app.m<br/>macOS graphical wrapper"]
        Engine --> DemoCLI
        WireH --> DemoCLI
        FreeRTOS --> DemoCLI
        DemoApp -->|"launches bundled CLI"| DemoCLI
        Cocoa --> DemoApp
    end

    CMake -.-> DemoCLI
    CMake -.-> DemoApp

    subgraph Tests["Automated verification"]
        CTests["tests/test_uart_dma.c<br/>parser, recovery, API and ISR tests"]
        PyTests["python/tests/test_ble_uart_app.py<br/>CRC, parser and transfer tests"]
        CTestRunner["CTest"]
        Core --> CTests
        PyProtocol --> PyTests
        CMake --> CTestRunner
        CTestRunner --> CTests
        CTestRunner --> DemoCLI
        CTestRunner --> PyTests
    end

    subgraph Documentation["Documentation pipeline"]
        Doxyfile["docs/Doxyfile.in"]
        Mainpage["docs/mainpage.dox"]
        Architecture["docs/architecture.md"]
        Generated["build/docs/html<br/>generated API documentation"]
        Readmes --> Mainpage
        Doxyfile --> Generated
        Mainpage --> Generated
        Wire --> Generated
        Core --> Generated
        Common --> Generated
        NativeClients --> Generated
        NativeServer --> Generated
        PythonApp --> Generated
        Demo --> Generated
        Doxygen --> Generated
        Architecture --> Readmes
    end

    CMake -.-> Generated
```
