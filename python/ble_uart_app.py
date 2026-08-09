#!/usr/bin/env python3
"""@file
@brief Tk desktop UART client UI for the BLE HCI transport.

Protocol construction and parsing live in `ble_protocol`; this module owns only
the application controller, background serial reader, and widgets.
"""

from __future__ import annotations

import queue
import sys
import threading
import time
from typing import Callable

from ble_protocol import (
    H4CrcStreamParser,
    add_crc,
    build_hci_command,
    build_test_transfer_frames,
    parse_hex,
)
from client_preferences import (
    DashboardSettings, load_port, load_settings, save_port, save_settings,
)
from serial_ports import available_ports
from dashboard_config import (
    BAUD_RATES, COMMAND_PRESETS, DMA_BUFFER_SIZES, RING_BUFFER_SIZES,
    SYSTEM_CLOCKS, TEST_TRANSFER_SIZES,
)
from dashboard_font import application_version, set_font_family
from dashboard_log import append_log_text
from dashboard_dropdown import RoundedCombo
from dashboard_pipeline import PipelineMonitor, PipelineTelemetry
from dashboard_ui import (
    RoundedActionButton, RoundedEntry, RoundedProgress, RoundedSurface,
    RoundedToggle, configure_styles,
)
from lpuart_sim import LpuartSimulator


def run_gui() -> None:
    """Create the Tk application and run its main event loop.

    @throws SystemExit If pyserial is not installed.
    """
    try:
        import serial
    except ImportError as error:
        raise SystemExit("pyserial is required. Run: python3 -m pip install -r python/requirements.txt") from error

    import tkinter as tk
    import tkinter.font as tkfont
    from tkinter import ttk


    class BleUartApp:
        """Coordinates Tk widgets, serial I/O, parsing, and test transfers."""

        def __init__(self, root: tk.Tk) -> None:
            """Initialize application state and construct the complete window.

            @param root Tk root window owned by the caller.
            """
            self.root = root
            self.root.title(f"BLE UART Client v{application_version()}")
            self.root.geometry("1120x960")
            self.root.minsize(980, 820)
            self.root.configure(background="#0A0F1C")
            self.serial_port: serial.Serial | None = None
            self.reader_thread: threading.Thread | None = None
            self.stop_reader = threading.Event()
            self.rx_queue: queue.Queue[tuple[str, object]] = queue.Queue()
            self.pipeline = PipelineTelemetry()
            self.port_scan_running = False
            self.parser = H4CrcStreamParser()
            self.test_identifier = 0
            self.handshake_identifier = 0
            self.handshake_pending = False
            self.handshake_status_text = ""
            self.connection_attempt = 0
            self.connecting = False
            self.log_line_count = 0

            default_port = "COM3" if sys.platform == "win32" else (
                "/dev/ttys006" if sys.platform == "darwin" else "/dev/ttyUSB0")
            self.saved_settings = load_settings("client")
            def selected_label(choices: tuple[tuple[str, int], ...],
                               value: int, fallback: str) -> str:
                """Map a persisted numeric setting back to its dropdown label."""
                return next((label for label, size in choices if size == value), fallback)
            self.port_var = tk.StringVar(value=load_port(default_port))
            self.system_clock_var = tk.StringVar(value=selected_label(
                SYSTEM_CLOCKS, self.saved_settings.clock, "20 MHz"))
            self.baud_var = tk.StringVar(value=selected_label(
                BAUD_RATES, self.saved_settings.baud, "4 M"))
            self.receiver_simulator = LpuartSimulator.configure(
                dict(SYSTEM_CLOCKS)[self.system_clock_var.get()],
                dict(BAUD_RATES)[self.baud_var.get()])
            self.status_var = tk.StringVar(value="Disconnected")
            self.preset_var = tk.StringVar(value=next(iter(COMMAND_PRESETS)))
            self.opcode_var = tk.StringVar(value="0C03")
            self.parameters_var = tk.StringVar()
            self.raw_var = tk.StringVar(value="01 03 0C 00")
            self.transfer_progress = tk.DoubleVar(value=0.0)
            self.transfer_status_var = tk.StringVar(value="0 / 0 bytes")
            self.bulk_size_var = tk.StringVar(value=selected_label(
                TEST_TRANSFER_SIZES, self.saved_settings.bulk, "2 MiB"))
            self.byte_irq_var = tk.BooleanVar(value=self.saved_settings.byte_irq)
            self.ring_buffer_var = tk.StringVar(value=selected_label(
                RING_BUFFER_SIZES, self.saved_settings.ring, "16 KiB"))
            self.dma_buffer_var = tk.StringVar(value=selected_label(
                DMA_BUFFER_SIZES, self.saved_settings.dma, "256 B"))
            self.ring_buffer_size = dict(RING_BUFFER_SIZES)[self.ring_buffer_var.get()]
            self.dma_buffer_size = dict(DMA_BUFFER_SIZES)[self.dma_buffer_var.get()]
            self.pipeline.ring_capacity = self.ring_buffer_size
            self.pipeline.dma_capacity = self.dma_buffer_size
            self.irq_count_var = tk.StringVar(value="IRQ calls: 0")
            self.tx_packets = 0
            self.rx_packets = 0
            self.tx_packets_var = tk.StringVar(value="0")
            self.rx_packets_var = tk.StringVar(value="0")
            self.irq_metric_var = tk.StringVar(value="0")
            self.transport_var = tk.StringVar(value="OFFLINE")
            self.irq_absolute_count = 0
            self.receive_byte_irq_active = False
            self.test_irq_active = False
            self.test_irq_calls = 0
            self.test_log_next_progress = 256 * 1024

            self._build_ui()
            self.refresh_ports()
            self.select_preset()
            self.log_notice("Ready. Start ble_uart_server and enter its printed port.")
            self.root.after(16, self.drain_reader_queue)
            self.root.protocol("WM_DELETE_WINDOW", self.close)


        def _build_ui(self) -> None:
            """Create the shared telemetry-first client dashboard layout."""
            configure_styles(self.root)
            container = ttk.Frame(self.root, style="App.TFrame")
            container.pack(fill=tk.BOTH, expand=True)

            tk.Label(container,
                     text=f"BLE UART HCI Client  v{application_version()}",
                     background="#0A0F1C", foreground="#E8EFF9",
                     font=(preferred_family, -28, "bold")).place(x=28, y=17)
            tk.Label(container,
                     text="PORTABLE H4 CONSOLE  /  CRC16-CCITT  /  LIVE PACKET TELEMETRY",
                     background="#0A0F1C", foreground="#8F9EB5",
                     font=(preferred_family, -12, "bold")).place(x=30, y=61)

            metrics = ttk.Frame(container, style="App.TFrame")
            metrics.place(x=24, y=100, width=1072, height=84)
            for column in range(4):
                metrics.columnconfigure(column, weight=1, uniform="metric")

            def metric_card(column: int, label: str, value: tk.StringVar,
                            accent: str, reset: Callable[[], None] | None) -> None:
                surface = RoundedSurface(metrics, fill="#1B253B", border="#1B253B",
                                         style="Card.TFrame", radius=13,
                                         content_padding=(16, 5))
                card = surface.content
                surface.grid(row=0, column=column, sticky=tk.EW,
                             padx=(0 if column == 0 else 6, 0 if column == 3 else 6))
                surface.configure(height=84)
                surface.grid_propagate(False)
                stripe = tk.Frame(card, width=4, height=55, background=accent)
                stripe.pack(side=tk.LEFT, padx=(0, 12))
                content = ttk.Frame(card, style="Card.TFrame")
                content.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
                label_widget = ttk.Label(content, text=label, style="CardLabel.TLabel")
                label_widget.pack(anchor=tk.W)
                value_widget = ttk.Label(content, textvariable=value, style="CardValue.TLabel")
                value_widget.pack(anchor=tk.W, pady=(5, 0))
                if reset is not None:
                    def reset_metric(_event: object = None) -> None:
                        reset()

                    for widget in (surface, surface._canvas, card, stripe, content,
                                   label_widget, value_widget):
                        widget.configure(cursor="pointinghand")
                        widget.bind("<ButtonRelease-1>", reset_metric)

            def reset_rx_packets() -> None:
                self.rx_packets = 0
                self.rx_packets_var.set("0")

            def reset_tx_packets() -> None:
                self.tx_packets = 0
                self.tx_packets_var.set("0")

            def reset_rx_irq_calls() -> None:
                self.irq_absolute_count = 0
                self.irq_metric_var.set("0")

            metric_card(0, "RX PACKETS", self.rx_packets_var, "#4C94FF", reset_rx_packets)
            metric_card(1, "TX PACKETS", self.tx_packets_var, "#2DD4BF", reset_tx_packets)
            metric_card(2, "RX IRQ CALLS", self.irq_metric_var, "#A78BFA",
                        reset_rx_irq_calls)
            metric_card(3, "TRANSPORT", self.transport_var, "#2DD4BF", None)

            def panel(title: str, bounds: tuple[int, int, int, int]) -> ttk.Frame:
                surface = RoundedSurface(container, fill="#141C2E", border="#31415C",
                                         style="Panel.TFrame", radius=13,
                                         content_padding=(18, 6))
                surface.place(x=bounds[0], y=bounds[1],
                              width=bounds[2], height=bounds[3])
                surface.pack_propagate(False)
                outer = surface.content
                ttk.Label(outer, text=title, style="Section.TLabel").pack(anchor=tk.W,
                                                                          pady=(0, 10))
                body = ttk.Frame(outer, style="Panel.TFrame")
                body.pack(fill=tk.BOTH, expand=True)
                return body

            connection = panel("CONNECTION", (24, 204, 1072, 136))
            ttk.Label(connection, text="Port", style="App.TLabel").place(
                x=0, y=3, width=50, height=38)
            self.port_combo = RoundedCombo(connection, self.port_var, editable=True)
            self.port_combo.place(x=52, y=3, width=426, height=38)
            self.port_combo.entry.bind("<FocusIn>", lambda _event: self.refresh_ports(), add="+")
            self.port_combo.entry.bind(
                "<FocusOut>", lambda _event: save_port(self.port_var.get()), add="+")
            self.port_combo.bind(
                "<<ComboboxSelected>>",
                lambda _event: save_port(self.port_var.get()), add="+")
            self.connect_button = RoundedActionButton(
                connection, "Connect", self.toggle_connection, width=170, height=38)
            self.connect_button.place(x=712, y=3)
            self.rx_mode_toggle = RoundedToggle(
                connection, "Per-byte IRQ", self.byte_irq_var,
                self.send_rx_mode_request)
            self.rx_mode_toggle.configure(state=tk.DISABLED)
            self.rx_mode_toggle.place(x=914, y=7, width=120, height=30)
            RoundedEntry(connection, self.status_var, readonly=True).place(
                x=0, y=51, width=166, height=34)
            ttk.Label(connection, text="Clock", style="Muted.TLabel").place(
                x=186, y=51, width=42, height=34)
            clock_combo = RoundedCombo(
                connection, self.system_clock_var,
                tuple(label for label, _value in SYSTEM_CLOCKS))
            clock_combo.place(x=232, y=51, width=104, height=34)
            clock_combo.bind("<<ComboboxSelected>>", self._configure_timing)
            ttk.Label(connection, text="Baud", style="Muted.TLabel").place(
                x=346, y=51, width=36, height=34)
            baud_combo = RoundedCombo(
                connection, self.baud_var,
                tuple(label for label, _value in BAUD_RATES))
            baud_combo.place(x=386, y=51, width=104, height=34)
            baud_combo.bind("<<ComboboxSelected>>", self._configure_timing)
            ttk.Label(connection, text="Ring", style="Muted.TLabel").place(
                x=504, y=51, width=36, height=34)
            ring_combo = RoundedCombo(
                connection, self.ring_buffer_var,
                tuple(label for label, _size in RING_BUFFER_SIZES))
            ring_combo.place(x=544, y=51, width=104, height=34)
            ring_combo.bind("<<ComboboxSelected>>", self._configure_receive_buffers)
            ttk.Label(connection, text="DMA", style="Muted.TLabel").place(
                x=662, y=51, width=36, height=34)
            dma_combo = RoundedCombo(
                connection, self.dma_buffer_var,
                tuple(label for label, _size in DMA_BUFFER_SIZES))
            dma_combo.place(x=702, y=51, width=104, height=34)
            dma_combo.bind("<<ComboboxSelected>>", self._configure_receive_buffers)
            RoundedActionButton(
                connection, "Defaults", self.restore_default_settings,
                width=206, height=34).place(x=826, y=51)
            ttk.Label(connection, text="Native clipboard shortcuts enabled",
                      style="Muted.TLabel").place(x=2, y=87, height=11)

            command = panel("HCI COMMAND BUILDER", (24, 360, 1072, 216))
            ttk.Label(command, text="Preset", style="App.TLabel").place(
                x=0, y=3, width=64, height=38)
            preset = RoundedCombo(command, self.preset_var, tuple(COMMAND_PRESETS))
            preset.place(x=68, y=3, width=360, height=38)
            preset.bind("<<ComboboxSelected>>", lambda _event: self.select_preset())
            ttk.Label(command, text="Opcode", style="App.TLabel").place(
                x=452, y=3, width=66, height=38)
            RoundedEntry(command, self.opcode_var).place(x=522, y=3, width=164, height=38)
            ttk.Label(command, text="Parameters", style="App.TLabel").place(
                x=0, y=58, width=132, height=38)
            RoundedEntry(command, self.parameters_var).place(
                x=136, y=58, width=646, height=38)
            self.send_command_button = RoundedActionButton(
                command, "Send command", self.send_command, width=226, height=38)
            self.send_command_button.configure(state=tk.DISABLED)
            self.send_command_button.place(x=806, y=58)
            ttk.Label(command, text="Raw H4 bytes", style="App.TLabel").place(
                x=0, y=118, width=132, height=38)
            RoundedEntry(command, self.raw_var).place(x=136, y=118, width=646, height=38)
            self.send_raw_button = RoundedActionButton(
                command, "Append CRC + send", self.send_raw, width=226, height=38)
            self.send_raw_button.configure(state=tk.DISABLED)
            self.send_raw_button.place(x=806, y=118)

            bulk = panel("TEST TRANSMISSIONS", (24, 596, 1072, 124))
            self.send_small_button = RoundedActionButton(
                bulk, "Send 128-byte packet", lambda: self.start_test_transfer(128),
                width=220, height=42)
            self.send_small_button.configure(state=tk.DISABLED)
            self.send_small_button.place(x=0, y=5)
            ttk.Label(bulk, text="Block size", style="App.TLabel").place(
                x=248, y=5, width=92, height=38)
            bulk_combo = RoundedCombo(
                bulk, self.bulk_size_var,
                tuple(label for label, _size in TEST_TRANSFER_SIZES))
            bulk_combo.place(x=348, y=5, width=132, height=42)
            bulk_combo.bind("<<ComboboxSelected>>", lambda _event: self._save_settings())
            self.send_large_button = RoundedActionButton(
                bulk, "Send selected block", self.send_selected_test_transfer,
                width=218, height=42)
            self.send_large_button.configure(state=tk.DISABLED)
            self.send_large_button.place(x=504, y=5)
            RoundedProgress(bulk, self.transfer_progress).place(
                x=748, y=5, width=284, height=16)
            ttk.Label(bulk, textvariable=self.transfer_status_var, style="Muted.TLabel").place(
                x=748, y=27, width=284, height=20)

            traffic = panel("LIVE UART TRAFFIC + PIPELINE HEADROOM",
                            (24, 740, 1072, 196))
            log_frame = tk.Frame(traffic, background="#141C2E",
                                 borderwidth=0, highlightthickness=0)
            log_frame.place(x=0, y=0, width=540, height=142)
            log_background = tk.Canvas(log_frame, background="#141C2E",
                                       borderwidth=0, highlightthickness=0)
            log_background.place(x=0, y=0, relwidth=1, relheight=1)

            def draw_log_background(_event: object = None) -> None:
                log_background.delete("all")
                width, height = log_frame.winfo_width() - 1, log_frame.winfo_height() - 1
                radius = min(8, max(1, height // 2))
                color = "#070B14"
                log_background.create_rectangle(radius, 0, width-radius, height,
                                                fill=color, outline=color)
                log_background.create_rectangle(0, radius, width, height-radius,
                                                fill=color, outline=color)
                for oval in ((0, 0, 2*radius, 2*radius),
                             (width-2*radius, 0, width, 2*radius),
                             (0, height-2*radius, 2*radius, height),
                             (width-2*radius, height-2*radius, width, height)):
                    log_background.create_oval(*oval, fill=color, outline=color)

            log_frame.bind("<Configure>", draw_log_background)
            log_content = tk.Frame(log_frame, background="#070B14",
                                   borderwidth=0, highlightthickness=0)
            log_content.place(x=6, y=4, relwidth=1, width=-12,
                              relheight=1, height=-8)
            log_scroll = ttk.Scrollbar(log_content, orient=tk.VERTICAL,
                                       style="Dark.Vertical.TScrollbar")
            log_scroll.pack(side=tk.RIGHT, fill=tk.Y)
            self.log = tk.Text(log_content, height=9, state=tk.DISABLED, wrap=tk.NONE,
                               background="#070B14", foreground="#BEC6D2",
                               insertbackground="#BEC6D2", relief=tk.FLAT,
                               highlightthickness=0, font=("Menlo", -12), padx=12, pady=8,
                               yscrollcommand=log_scroll.set)
            self.log.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
            log_scroll.configure(command=self.log.yview)
            self.log.tag_configure("TX", foreground="#64BEFF")
            self.log.tag_configure("RX", foreground="#69E196")
            self.log.tag_configure("ERROR", foreground="#FF7878")
            self.log.tag_configure("TEST", foreground="#BEC6D2")
            self.pipeline_monitor = PipelineMonitor(traffic, self.pipeline)
            self.pipeline_monitor.place(x=556, y=0, width=476, height=142)
            self.root.after(50, self._refresh_pipeline_monitor)

        def _refresh_pipeline_monitor(self) -> None:
            """Refresh the shared monitor at the native dashboard cadence."""
            self.pipeline_monitor.draw()
            self.root.after(50, self._refresh_pipeline_monitor)

        def _configure_timing(self, _event: object = None) -> None:
            """Apply the selected app-local clock and line-rate parameters."""
            clock = dict(SYSTEM_CLOCKS)[self.system_clock_var.get()]
            baud = dict(BAUD_RATES)[self.baud_var.get()]
            self.receiver_simulator = LpuartSimulator.configure(clock, baud)
            timing = self.receiver_simulator
            self.log_notice(
                f"RX  Timing: clock={clock} baud={baud} OSR={timing.osr} "
                f"SBR={timing.sbr} actual={timing.actual_baud}", "RX")
            self._save_settings()

        def _save_settings(self) -> None:
            """Persist every user-adjustable operating control."""
            self.saved_settings = DashboardSettings(
                clock=dict(SYSTEM_CLOCKS)[self.system_clock_var.get()],
                baud=dict(BAUD_RATES)[self.baud_var.get()],
                ring=dict(RING_BUFFER_SIZES)[self.ring_buffer_var.get()],
                dma=dict(DMA_BUFFER_SIZES)[self.dma_buffer_var.get()],
                bulk=dict(TEST_TRANSFER_SIZES)[self.bulk_size_var.get()],
                byte_irq=self.byte_irq_var.get())
            save_settings("client", self.saved_settings)

        def restore_default_settings(self) -> None:
            """Restore and immediately apply the documented defaults."""
            defaults = DashboardSettings()
            self.system_clock_var.set("20 MHz")
            self.baud_var.set("4 M")
            self.ring_buffer_var.set("16 KiB")
            self.dma_buffer_var.set("256 B")
            self.bulk_size_var.set("2 MiB")
            self.byte_irq_var.set(False)
            self.receive_byte_irq_active = False
            self.ring_buffer_size = defaults.ring
            self.dma_buffer_size = defaults.dma
            self.receiver_simulator = LpuartSimulator.configure(
                defaults.clock, defaults.baud)
            self.pipeline.reset()
            with self.pipeline.lock:
                self.pipeline.queue_capacity = 16
                self.pipeline.ring_capacity = defaults.ring
                self.pipeline.dma_capacity = defaults.dma
            self.transport_var.set(
                "DMA + IDLE" if self.serial_port is not None else "OFFLINE")
            self._save_settings()
            self.log_notice("SETTINGS  Restored application defaults")

        def log_line(self, direction: str, data: bytes, detail: str = "") -> None:
            """Append one formatted TX or RX record to the traffic view.

            @param direction Short direction label such as `TX` or `RX`.
            @param data Wire bytes to display.
            @param detail Optional protocol description.
            """
            suffix = f"  {detail}" if detail else ""
            self._append_log_text(f"{direction:<5} {data.hex(' ').upper()}{suffix}", direction)

        def _append_log_text(self, message: str, kind: str) -> None:
            """Append and color one line while preserving manual scroll position."""
            self.log_line_count = append_log_text(
                self.log, self.log_line_count, message, kind)

        def _configure_receive_buffers(self, _event: object = None) -> None:
            """Apply the selected local ring capacity and DMA transaction size."""
            self.ring_buffer_size = dict(RING_BUFFER_SIZES)[self.ring_buffer_var.get()]
            self.dma_buffer_size = dict(DMA_BUFFER_SIZES)[self.dma_buffer_var.get()]
            with self.pipeline.lock:
                self.pipeline.queue_capacity = 16
                self.pipeline.ring_capacity = self.ring_buffer_size
                self.pipeline.dma_capacity = self.dma_buffer_size
            self._save_settings()
            self.log_notice(
                f"RX  Buffers: ring={self.ring_buffer_size} bytes, "
                f"DMA={self.dma_buffer_size} bytes", "RX")

        def log_notice(self, message: str, kind: str = "TEST") -> None:
            """Append a non-packet status line to the rolling traffic view."""
            self._append_log_text(message, kind)

        def refresh_ports(self) -> None:
            """Refresh devices asynchronously so an OS serial driver cannot block Tk."""
            if self.port_scan_running:
                return
            self.port_scan_running = True

            def discover() -> None:
                """Run pyserial/IOKit enumeration outside Tk's event thread."""
                try:
                    self.rx_queue.put(("ports", list(available_ports())))
                except Exception as error:  # A platform backend may raise driver-specific errors.
                    self.rx_queue.put(("ports_error", error))

            threading.Thread(target=discover, daemon=True).start()

        def toggle_connection(self) -> None:
            """Connect the selected port or disconnect the active port.

            Pseudo-terminals fall back to a simulated 115,200 setting when their
            driver rejects the user-selected logical baud rate.
            """
            if self.serial_port is not None:
                self.disconnect()
                return
            if self.connecting:
                self.connection_attempt += 1
                self.connecting = False
                self.connect_button.configure(text="Connect")
                self.status_var.set("Connection attempt cancelled")
                self.transport_var.set("OFFLINE")
                return
            port_name = self.port_var.get()
            save_port(port_name)
            try:
                requested_baud = dict(BAUD_RATES)[self.baud_var.get()]
            except ValueError:
                self.status_var.set("Invalid baud rate")
                self.log_notice("ERROR  Baud rate must be a decimal number", "ERROR")
                return
            if not 0 < requested_baud <= 4_000_000:
                self.status_var.set("Baud must be between 1 and 4000000")
                self.log_notice("ERROR  Baud must be between 1 and 4000000", "ERROR")
                return
            self.connection_attempt += 1
            attempt = self.connection_attempt
            self.connecting = True
            self.connect_button.configure(text="Cancel")
            self.status_var.set(f"Opening {port_name}...")
            self.transport_var.set("CONNECTING")

            def open_port() -> None:
                """Open a driver-backed serial device without blocking Tk."""
                opened: serial.Serial | None = None
                simulated_baud = False
                error: Exception | None = None
                try:
                    opened = serial.Serial(port_name, requested_baud, timeout=0.1,
                                           write_timeout=2.0)
                except (ValueError, OSError, serial.SerialException) as first_error:
                    error = first_error
                    if port_name.startswith(("/dev/ttys", "/dev/pts/")):
                        try:
                            opened = serial.Serial(port_name, 115200, timeout=0.1,
                                                   write_timeout=2.0)
                            simulated_baud = True
                            error = None
                        except (ValueError, OSError, serial.SerialException) as fallback_error:
                            error = fallback_error
                self.rx_queue.put(("connect_result",
                                   (attempt, opened, requested_baud,
                                    simulated_baud, error)))

            threading.Thread(target=open_port, daemon=True).start()
            self.root.after(3000, lambda expected=attempt: self._connection_open_timeout(expected))

        def _connection_open_timeout(self, expected_attempt: int) -> None:
            """Return control when an operating-system serial open blocks."""
            if self.connecting and self.connection_attempt == expected_attempt:
                self.connection_attempt += 1
                self.connecting = False
                self.connect_button.configure(text="Connect")
                self.status_var.set("Unable to open UART: driver did not respond")
                self.transport_var.set("OFFLINE")
                self.log_notice("ERROR  UART open timed out; check the selected port", "ERROR")

        def _finish_connection_open(self, port: serial.Serial, requested_baud: int,
                                    simulated_baud: bool) -> None:
            """Start verification after the background serial open succeeds."""
            self.connecting = False
            self.serial_port = port
            self.pipeline.reset()
            self.stop_reader.clear()
            self.parser = H4CrcStreamParser()
            self.reader_thread = threading.Thread(target=self.read_serial, daemon=True)
            self.reader_thread.start()
            self.connect_button.configure(text="Disconnect")
            self.rx_mode_toggle.configure(state=tk.DISABLED)
            self.send_command_button.configure(state=tk.DISABLED)
            self.send_raw_button.configure(state=tk.DISABLED)
            self.send_small_button.configure(state=tk.DISABLED)
            self.send_large_button.configure(state=tk.DISABLED)
            suffix = f" (simulating {requested_baud:,} baud on PTY)" if simulated_baud else ""
            self.handshake_status_text = f"Connected to {self.serial_port.port}{suffix}"
            self.status_var.set(f"Verifying BLE UART server on {self.serial_port.port}...")
            self.transport_var.set("VERIFYING")
            self.log_notice(
                f"Opened {self.serial_port.port} at {requested_baud} baud; verifying server")
            self.handshake_identifier = (self.handshake_identifier + 1) & 0xFFFF or 1
            identifier = self.handshake_identifier
            self.handshake_pending = True
            probe = next(build_test_transfer_frames(128, identifier))
            self.write_frame(probe, f"128-byte server verification probe (id={identifier})")
            self.root.after(2500, lambda expected=identifier: self._handshake_timeout(expected))

        def disconnect(self, error_message: str | None = None) -> None:
            """Close the serial device and optionally preserve a visible error."""
            self.stop_reader.set()
            self.connection_attempt += 1
            self.connecting = False
            self.handshake_pending = False
            port = self.serial_port
            self.serial_port = None
            self.pipeline.reset()
            if port is not None:
                port_name = port.port
                port.close()
                if error_message is None:
                    self.log_notice(f"Disconnected from {port_name}")
            self.connect_button.configure(text="Connect")
            self.rx_mode_toggle.configure(state=tk.DISABLED)
            self.send_command_button.configure(state=tk.DISABLED)
            self.send_raw_button.configure(state=tk.DISABLED)
            self.send_small_button.configure(state=tk.DISABLED)
            self.send_large_button.configure(state=tk.DISABLED)
            if error_message is None:
                self.status_var.set("Disconnected")
            else:
                self.status_var.set(error_message)
                self.log_notice(f"ERROR  {error_message}", "ERROR")
            self.transport_var.set("OFFLINE")

        def _handshake_timeout(self, expected_identifier: int) -> None:
            """Reject an open port that did not acknowledge the current probe."""
            if (self.serial_port is not None and self.handshake_pending
                    and self.handshake_identifier == expected_identifier):
                self.disconnect(
                    "Wrong UART: selected port is not a BLE UART server (no acknowledgement)")

        def _complete_handshake(self) -> None:
            """Enable client controls after a matching server acknowledgement."""
            self.handshake_pending = False
            self.status_var.set(self.handshake_status_text)
            self.receive_byte_irq_active = self.byte_irq_var.get()
            self.transport_var.set(
                "BYTE IRQ" if self.receive_byte_irq_active else "DMA + IDLE")
            self.rx_mode_toggle.configure(state=tk.NORMAL)
            self.send_command_button.configure(state=tk.NORMAL)
            self.send_raw_button.configure(state=tk.NORMAL)
            self.send_small_button.configure(state=tk.NORMAL)
            self.send_large_button.configure(state=tk.NORMAL)
            self.log_notice("VERIFY  BLE UART server acknowledged the 128-byte probe")

        def read_serial(self) -> None:
            """Background worker that forwards serial reads to the Tk-safe queue."""
            while not self.stop_reader.is_set():
                port = self.serial_port
                if port is None:
                    return
                try:
                    data = port.read(1)
                    waiting = min(port.in_waiting,
                                  self.dma_buffer_size - len(data))
                    if waiting:
                        data += port.read(waiting)
                    if data:
                        self.rx_queue.put(("data", (data, time.monotonic_ns())))
                except serial.SerialException as error:
                    if not self.stop_reader.is_set():
                        self.rx_queue.put(("error", error))
                    return

        def drain_reader_queue(self) -> None:
            """Process queued receive, error, and transfer-progress notifications."""
            try:
                while True:
                    kind, value = self.rx_queue.get_nowait()
                    if kind == "error":
                        if self.handshake_pending:
                            self.disconnect(
                                f"Wrong UART: BLE UART verification failed ({value})")
                        else:
                            self.log_notice(f"ERROR  {value}", "ERROR")
                            self.disconnect()
                    elif kind == "connect_result":
                        result = value if isinstance(value, tuple) else ()
                        if len(result) == 5:
                            attempt, opened, requested_baud, simulated_baud, error = result
                            if attempt != self.connection_attempt or not self.connecting:
                                if opened is not None:
                                    opened.close()
                            elif error is not None or opened is None:
                                self.connecting = False
                                self.connect_button.configure(text="Connect")
                                self.status_var.set(f"Unable to open UART: {error}")
                                self.transport_var.set("OFFLINE")
                                self.log_notice(f"ERROR  Unable to open UART: {error}", "ERROR")
                            else:
                                self._finish_connection_open(
                                    opened, int(requested_baud), bool(simulated_baud))
                    elif kind == "ports":
                        self.port_scan_running = False
                        ports = value if isinstance(value, list) else []
                        devices = [device for device, _description in ports]
                        self.port_combo["values"] = devices
                        if devices and not self.port_var.get():
                            self.port_var.set(devices[0])
                    elif kind == "ports_error":
                        self.port_scan_running = False
                        self.log_notice(f"ERROR  Serial-port discovery failed: {value}", "ERROR")
                    elif kind == "data":
                        packet = value if isinstance(value, tuple) else (b"", time.monotonic_ns())
                        incoming = packet[0] if isinstance(packet[0], bytes) else b""
                        queued_at_ns = int(packet[1])
                        byte_irq_active = self.receive_byte_irq_active
                        completed = self.parser.feed(incoming)
                        for frame in completed:
                            wire_size = len(frame.h4_packet) + 2
                            frame_irqs = (wire_size if byte_irq_active else
                                          (wire_size + self.dma_buffer_size - 1)
                                          // self.dma_buffer_size)
                            self.irq_absolute_count += frame_irqs
                            self.pipeline.record_arrival(wire_size)
                            if wire_size > self.ring_buffer_size:
                                with self.pipeline.lock:
                                    self.pipeline.ring_drops += 1
                                self.log_notice(
                                    f"ERROR  RX ring overwrite: frame={wire_size} "
                                    f"capacity={self.ring_buffer_size}", "ERROR")
                                continue
                            self.pipeline.record_produced(len(frame.h4_packet), 1)
                            self.pipeline.record_consumed(
                                len(frame.h4_packet), 0, queued_at_ns)
                            self.rx_packets += 1
                            if not frame.crc_valid:
                                with self.pipeline.lock:
                                    self.pipeline.crc_drops += 1
                            completion = frame.test_completion if frame.crc_valid else None
                            if self.handshake_pending and completion is not None:
                                identifier, status, total_size = completion
                                if identifier == self.handshake_identifier:
                                    if status == 0 and total_size == 128:
                                        self._complete_handshake()
                                    else:
                                        self.disconnect(
                                            "Wrong UART: BLE UART acknowledgement did not match the probe")
                                        break
                            crc_status = "CRC OK" if frame.crc_valid else (
                                f"CRC BAD (got {frame.received_crc:04X}, expected {frame.computed_crc:04X})"
                            )
                            flags = frame.test_transfer_flags
                            irq_detail = ""
                            if flags is not None and flags & 0x01:
                                self.test_irq_active = True
                                self.test_irq_calls = 0
                                self.test_log_next_progress = 256 * 1024
                            if flags is not None and self.test_irq_active:
                                self.test_irq_calls += frame_irqs
                            if flags is not None and self.test_irq_active and flags & 0x02:
                                transmission_irqs = self.test_irq_calls
                                self.test_irq_active = False
                                irq_detail = (
                                    f", IRQ complete: {transmission_irqs} calls; "
                                    f"app total={self.irq_absolute_count}")
                            elif flags is None:
                                transmission_irqs = frame_irqs
                                irq_detail = (
                                    f", IRQ complete: {transmission_irqs} calls; "
                                    f"app total={self.irq_absolute_count}")
                            transfer = frame.test_transfer_info
                            if transfer is not None and transfer[0] > 128:
                                total, offset, chunk_size, transfer_flags = transfer
                                received = offset + chunk_size
                                if transfer_flags & 0x01:
                                    self.log_notice(f"RX  TEST start: {total} bytes", "RX")
                                if transfer_flags & 0x02:
                                    self.log_notice(
                                        f"RX  TEST complete: {received}/{total} bytes"
                                        f"{irq_detail}", "RX")
                                elif received >= self.test_log_next_progress:
                                    self.log_notice(
                                        f"RX  TEST progress: {received}/{total} bytes "
                                        f"({received * 100 // total}%)", "RX")
                                    self.test_log_next_progress += 256 * 1024
                            else:
                                self.log_line(
                                    "RX", frame.h4_packet
                                    + frame.received_crc.to_bytes(2, "big"),
                                    f"[{crc_status}, {frame.summary}{irq_detail}]")
                        self.rx_packets_var.set(str(self.rx_packets))
                        self.irq_count_var.set(f"IRQ calls: {self.irq_absolute_count}")
                        self.irq_metric_var.set(str(self.irq_absolute_count))
                    elif kind == "tx_packets":
                        self.tx_packets += int(value)
                        self.tx_packets_var.set(str(self.tx_packets))
                    elif kind == "tx_progress":
                        sent, total = value if isinstance(value, tuple) else (0, 0)
                        self.transfer_progress.set(sent / total if total else 0.0)
                        self.transfer_status_var.set(f"{sent} / {total} bytes")
                        self.log_notice(f"TEST  Sent {sent}/{total} bytes ({sent * 100 // total}%)")
                    elif kind == "tx_gauge":
                        sent, total = value if isinstance(value, tuple) else (0, 0)
                        self.transfer_progress.set(sent / total if total else 0.0)
                        self.transfer_status_var.set(f"{sent} / {total} bytes")
                    elif kind == "tx_done":
                        total = int(value)
                        self.transfer_progress.set(1.0)
                        self.transfer_status_var.set(f"{total} / {total} bytes")
                        self.log_notice(f"TEST  All {total} bytes sent; waiting for server verification")
                        if self.serial_port is not None:
                            self.send_small_button.configure(state=tk.NORMAL)
                            self.send_large_button.configure(state=tk.NORMAL)
            except queue.Empty:
                pass
            self.root.after(16, self.drain_reader_queue)

        def write_frame(self, frame: bytes, detail: str) -> None:
            """Synchronously write one complete frame and log it.

            @param frame H4 packet and CRC trailer.
            @param detail Human-readable protocol description.
            """
            if self.serial_port is None:
                self.log_notice("ERROR  Connect to a serial port first", "ERROR")
                return
            try:
                self.serial_port.write(frame)
                self.serial_port.flush()
                self.tx_packets += 1
                self.tx_packets_var.set(str(self.tx_packets))
                self.log_line("TX", frame[:-2], f"[CRC {frame[-2:].hex().upper()}] {detail}")
            except serial.SerialException as error:
                self.log_notice(f"ERROR  Serial write failed: {error}", "ERROR")
                if self.handshake_pending:
                    self.disconnect(
                        "Wrong UART: BLE UART verification probe could not be sent")
                else:
                    self.disconnect()

        def select_preset(self) -> None:
            """Copy the selected command preset into editable opcode and parameter fields."""
            opcode, parameters = COMMAND_PRESETS[self.preset_var.get()]
            self.opcode_var.set(f"{opcode:04X}")
            self.parameters_var.set(parameters.hex(" ").upper())

        def send_command(self) -> None:
            """Validate the command fields, construct a frame, and transmit it."""
            try:
                opcode = int(self.opcode_var.get().replace("0x", ""), 16)
                parameters = parse_hex(self.parameters_var.get())
                frame = build_hci_command(opcode, parameters)
            except ValueError as error:
                self.log_notice(f"ERROR  {error}", "ERROR")
                return
            self.write_frame(frame, f"HCI command 0x{opcode:04X}; CRC {frame[-2:].hex().upper()}")

        def send_raw(self) -> None:
            """Parse raw H4 text, append its CRC, and transmit the resulting frame."""
            try:
                payload = parse_hex(self.raw_var.get())
                if not payload:
                    raise ValueError("Enter at least one H4 byte.")
                frame = add_crc(payload)
            except ValueError as error:
                self.log_notice(f"ERROR  {error}", "ERROR")
                return
            self.write_frame(frame, f"raw H4; CRC {frame[-2:].hex().upper()}")

        def start_test_transfer(self, total_size: int) -> None:
            """Start a background deterministic test transfer.

            @param total_size Logical number of generated payload bytes.
            """
            if self.serial_port is None:
                self.log_notice("ERROR  Connect before sending test data", "ERROR")
                return
            self.test_identifier = (self.test_identifier + 1) & 0xFFFF or 1
            identifier = self.test_identifier
            transfer_baud = dict(BAUD_RATES)[self.baud_var.get()]
            self.transfer_progress.set(0.0)
            self.transfer_status_var.set(f"0 / {total_size} bytes")
            self.send_small_button.configure(state=tk.DISABLED)
            self.send_large_button.configure(state=tk.DISABLED)
            self.log_notice(f"TEST  Sending {total_size} bytes (id={identifier})")

            def send_worker() -> None:
                """Write generated test frames and post progress to the UI queue."""
                sent = 0
                started_at = time.monotonic()
                next_progress = 256 * 1024
                gauge_target = transfer_baud // 8 // 60
                gauge_step = max(230, gauge_target // 230 * 230)
                batch = bytearray()
                batch_packets = 0
                batch_payload = 0
                try:
                    for frame in build_test_transfer_frames(total_size, identifier):
                        port = self.serial_port
                        if port is None:
                            return
                        batch.extend(frame)
                        batch_packets += 1
                        chunk_size = int.from_bytes(frame[21:23], "little")
                        sent += chunk_size
                        batch_payload += chunk_size
                        if (batch_payload >= gauge_step or
                                len(batch) >= 64 * 1024 or sent == total_size):
                            deadline = started_at + sent * 8 / transfer_baud
                            delay = deadline - time.monotonic()
                            if delay > 0:
                                time.sleep(delay)
                            port.write(batch)
                            batch.clear()
                            self.rx_queue.put(("tx_packets", batch_packets))
                            self.rx_queue.put(("tx_gauge", (sent, total_size)))
                            batch_packets = 0
                            batch_payload = 0
                        if sent >= next_progress:
                            self.rx_queue.put(("tx_progress", (sent, total_size)))
                            next_progress += 256 * 1024
                    port = self.serial_port
                    if port is not None:
                        port.flush()
                    self.rx_queue.put(("tx_done", total_size))
                except serial.SerialException as error:
                    self.rx_queue.put(("error", error))

            threading.Thread(target=send_worker, daemon=True).start()

        def send_selected_test_transfer(self) -> None:
            """Start the large transfer selected by the shared-size dropdown."""
            selected = self.bulk_size_var.get()
            sizes = dict(TEST_TRANSFER_SIZES)
            self.start_test_transfer(sizes[selected])

        def send_rx_mode_request(self) -> None:
            """Apply this app's local receive interrupt implementation."""
            if self.serial_port is None:
                return
            self.receive_byte_irq_active = self.byte_irq_var.get()
            self.transport_var.set(
                "BYTE IRQ" if self.receive_byte_irq_active else "DMA + IDLE")
            self.log_notice(
                "RX  Local receive mode: "
                + ("per-byte IRQ" if self.receive_byte_irq_active else "DMA batches"), "RX")
            self._save_settings()

        def close(self) -> None:
            """Release the serial resource and destroy the Tk root window."""
            save_port(self.port_var.get())
            self._save_settings()
            self.disconnect()
            self.root.destroy()

    root = tk.Tk()
    available_families = set(tkfont.families(root))
    preferred_family = "Space Grotesk" if "Space Grotesk" in available_families else (
        "Segoe UI" if sys.platform == "win32" else
        "Avenir Next" if sys.platform == "darwin" else "DejaVu Sans")
    set_font_family(preferred_family)
    for named_font in ("TkDefaultFont", "TkTextFont", "TkMenuFont",
                       "TkCaptionFont", "TkSmallCaptionFont"):
        tkfont.nametofont(named_font).configure(family=preferred_family, size=-15)
    BleUartApp(root)
    root.mainloop()


if __name__ == "__main__":
    run_gui()
