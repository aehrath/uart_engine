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
from pathlib import Path

from ble_protocol import (
    COMMAND_PRESETS,
    H4CrcStreamParser,
    RX_MODE_BYTE_IRQ,
    RX_MODE_DMA,
    RX_MODE_QUERY,
    TEST_TRANSFER_SIZES,
    add_crc,
    build_hci_command,
    build_rx_mode_command,
    build_test_transfer_frames,
    parse_hex,
)
from serial_ports import available_ports


def application_version() -> str:
    """Read the repository-wide semantic application version."""
    try:
        return (Path(__file__).resolve().parents[1] / "VERSION").read_text(
            encoding="utf-8").strip()
    except OSError:
        return "unknown"


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
            self.root.geometry("960x780")
            self.root.minsize(760, 600)
            self.root.configure(background="#F5F7FA")
            self.serial_port: serial.Serial | None = None
            self.reader_thread: threading.Thread | None = None
            self.stop_reader = threading.Event()
            self.rx_queue: queue.Queue[tuple[str, object]] = queue.Queue()
            self.port_scan_running = False
            self.parser = H4CrcStreamParser()
            self.test_identifier = 0
            self.log_line_count = 0

            default_port = "COM3" if sys.platform == "win32" else (
                "/dev/ttys006" if sys.platform == "darwin" else "/dev/ttyUSB0")
            self.port_var = tk.StringVar(value=default_port)
            self.baud_var = tk.StringVar(value="4000000")
            self.status_var = tk.StringVar(value="Disconnected")
            self.preset_var = tk.StringVar(value=next(iter(COMMAND_PRESETS)))
            self.opcode_var = tk.StringVar(value="0C03")
            self.parameters_var = tk.StringVar()
            self.raw_var = tk.StringVar(value="01 03 0C 00")
            self.transfer_progress = tk.DoubleVar(value=0.0)
            self.transfer_status_var = tk.StringVar(value="0 / 0 bytes")
            self.bulk_size_var = tk.StringVar(value="2 MiB")
            self.byte_irq_var = tk.BooleanVar(value=False)
            self.irq_count_var = tk.StringVar(value="IRQ calls: 0")
            self.irq_absolute_count = 0
            self.receive_byte_irq_active = False
            self.test_irq_active = False
            self.test_irq_calls = 0
            self.test_log_next_progress = 256 * 1024

            self._build_ui()
            self.refresh_ports()
            self.select_preset()
            self.log_notice("Ready. Start ble_uart_server and enter its printed port.")
            self.root.after(40, self.drain_reader_queue)
            self.root.protocol("WM_DELETE_WINDOW", self.close)

        def _configure_styles(self) -> None:
            """Configure a cross-platform palette matching the native C client."""
            style = ttk.Style(self.root)
            if "clam" in style.theme_names():
                style.theme_use("clam")
            style.configure("App.TFrame", background="#F5F7FA")
            style.configure("App.TLabel", background="#F5F7FA", foreground="#5A6478")
            style.configure("App.TLabelframe", background="#F5F7FA", bordercolor="#A0A8B5")
            style.configure("App.TLabelframe.Label", background="#F5F7FA",
                            foreground="#192337")
            style.configure("Status.TLabel", background="#FFFFFF", foreground="#192337",
                            relief=tk.SUNKEN, padding=(8, 5))
            style.configure("Transfer.Horizontal.TProgressbar", troughcolor="#DCE1E8",
                            background="#4691EB", bordercolor="#969EAC")

        def _build_ui(self) -> None:
            """Create the same connection, command, transfer, and log layout as the C UI."""
            self._configure_styles()
            container = ttk.Frame(self.root, padding=(20, 16), style="App.TFrame")
            container.pack(fill=tk.BOTH, expand=True)

            tk.Label(container,
                     text=f"BLE UART HCI Client  v{application_version()}",
                     background="#F5F7FA", foreground="#192337",
                     font=("Helvetica", 20, "bold")).pack(anchor=tk.W)
            tk.Label(container,
                     text="Portable Python UI • H4 framing • CRC16-CCITT • live receive parser",
                     background="#F5F7FA", foreground="#5A6478",
                     font=("Helvetica", 11)).pack(anchor=tk.W, pady=(2, 12))

            connection = ttk.LabelFrame(container, text="Serial connection", padding=(14, 10),
                                        style="App.TLabelframe")
            connection.pack(fill=tk.X)
            ttk.Label(connection, text="Port", style="App.TLabel").grid(row=0, column=0,
                                                                          sticky=tk.W)
            self.port_combo = ttk.Combobox(connection, textvariable=self.port_var, width=42)
            self.port_combo.grid(row=0, column=1, padx=(8, 18), sticky=tk.EW)
            self.port_combo.bind("<Button-1>", lambda _event: self.refresh_ports())
            ttk.Label(connection, text="Baud", style="App.TLabel").grid(row=0, column=2)
            ttk.Combobox(connection, textvariable=self.baud_var, width=10,
                         values=("9600", "57600", "115200", "460800", "921600",
                                 "1000000", "2000000", "3000000", "4000000")).grid(
                                     row=0, column=3, padx=8)
            self.connect_button = ttk.Button(connection, text="Connect", command=self.toggle_connection)
            self.connect_button.grid(row=0, column=4, ipadx=34)
            ttk.Label(connection,
                      text="Native copy/paste shortcuts • hardware baud rates up to 4000000",
                      style="App.TLabel").grid(row=1, column=0, columnspan=3,
                                                sticky=tk.W, pady=(7, 4))
            ttk.Label(connection, textvariable=self.irq_count_var,
                      style="App.TLabel").grid(row=1, column=3, sticky=tk.E,
                                                pady=(7, 4), padx=(0, 12))
            self.rx_mode_toggle = ttk.Checkbutton(
                connection, text="Per-byte IRQ", variable=self.byte_irq_var,
                command=self.send_rx_mode_request, state=tk.DISABLED)
            self.rx_mode_toggle.grid(row=1, column=4, sticky=tk.E, pady=(7, 4))
            ttk.Label(connection, textvariable=self.status_var, style="Status.TLabel").grid(
                row=2, column=0, columnspan=5, sticky=tk.EW)
            connection.columnconfigure(1, weight=1)

            command = ttk.LabelFrame(container, text="HCI command", padding=(14, 10),
                                     style="App.TLabelframe")
            command.pack(fill=tk.X, pady=(12, 0))
            ttk.Label(command, text="Preset", style="App.TLabel").grid(row=0, column=0,
                                                                         sticky=tk.W)
            preset = ttk.Combobox(command, textvariable=self.preset_var,
                                  values=tuple(COMMAND_PRESETS), state="readonly", width=28)
            preset.grid(row=0, column=1, padx=(8, 24), sticky=tk.EW)
            preset.bind("<<ComboboxSelected>>", lambda _event: self.select_preset())
            ttk.Label(command, text="Opcode", style="App.TLabel").grid(row=0, column=2)
            ttk.Entry(command, textvariable=self.opcode_var, width=10).grid(row=0, column=3, padx=8)
            ttk.Label(command, text="Parameters (hex)", style="App.TLabel").grid(
                row=1, column=0, sticky=tk.W, pady=(10, 0))
            ttk.Entry(command, textvariable=self.parameters_var).grid(row=1, column=1, columnspan=3,
                                                                       padx=8, pady=(10, 0), sticky=tk.EW)
            ttk.Button(command, text="Send command", command=self.send_command).grid(
                row=1, column=4, padx=(12, 0), pady=(10, 0), ipadx=14)
            ttk.Label(command, text="Raw H4 (hex)", style="App.TLabel").grid(
                row=2, column=0, sticky=tk.W, pady=(10, 0))
            ttk.Entry(command, textvariable=self.raw_var).grid(
                row=2, column=1, columnspan=3, padx=8, pady=(10, 0), sticky=tk.EW)
            ttk.Button(command, text="Append CRC + send", command=self.send_raw).grid(
                row=2, column=4, padx=(12, 0), pady=(10, 0), ipadx=5)
            command.columnconfigure(1, weight=1)

            bulk = ttk.LabelFrame(container, text="Bulk test messages", padding=(14, 10),
                                  style="App.TLabelframe")
            bulk.pack(fill=tk.X, pady=(12, 0))
            ttk.Button(bulk, text="Send 128-byte message",
                       command=lambda: self.start_test_transfer(128)).grid(row=0, column=0,
                                                                           ipadx=8)
            ttk.Label(bulk, text="Large block", style="App.TLabel").grid(
                row=0, column=1, padx=(12, 6))
            ttk.Combobox(bulk, textvariable=self.bulk_size_var,
                         values=tuple(label for label, _size in TEST_TRANSFER_SIZES),
                         state="readonly", width=9).grid(row=0, column=2)
            ttk.Button(bulk, text="Send selected block",
                       command=self.send_selected_test_transfer).grid(
                           row=0, column=3, padx=(12, 18), ipadx=5)
            ttk.Progressbar(bulk, variable=self.transfer_progress, maximum=1.0,
                            style="Transfer.Horizontal.TProgressbar").grid(
                                row=0, column=4, sticky=tk.EW)
            ttk.Label(bulk, textvariable=self.transfer_status_var, style="App.TLabel").grid(
                row=1, column=4, sticky=tk.W, pady=(5, 0))
            bulk.columnconfigure(4, weight=1)

            traffic = ttk.LabelFrame(container, text="UART traffic", padding=(12, 10),
                                     style="App.TLabelframe")
            traffic.pack(fill=tk.BOTH, expand=True, pady=(12, 0))
            self.log = tk.Text(traffic, height=13, state=tk.DISABLED, wrap=tk.NONE,
                               background="#141923", foreground="#BEC6D2",
                               insertbackground="#BEC6D2", relief=tk.FLAT,
                               font=("TkFixedFont", 10), padx=10, pady=8)
            self.log.pack(fill=tk.BOTH, expand=True)
            self.log.tag_configure("TX", foreground="#64BEFF")
            self.log.tag_configure("RX", foreground="#69E196")
            self.log.tag_configure("ERROR", foreground="#FF7878")
            self.log.tag_configure("TEST", foreground="#BEC6D2")

        def log_line(self, direction: str, data: bytes, detail: str = "") -> None:
            """Append one formatted TX or RX record to the traffic view.

            @param direction Short direction label such as `TX` or `RX`.
            @param data Wire bytes to display.
            @param detail Optional protocol description.
            """
            suffix = f"  {detail}" if detail else ""
            self._append_log_text(f"{direction:<5} {data.hex(' ').upper()}{suffix}", direction)

        def _append_log_text(self, message: str, kind: str) -> None:
            """Append and color one line while retaining the C UI's 13-line window."""
            self.log.configure(state=tk.NORMAL)
            self.log.insert(tk.END, f"{message}\n", kind)
            self.log_line_count += 1
            if self.log_line_count > 13:
                self.log.delete("1.0", "2.0")
                self.log_line_count -= 1
            self.log.see(tk.END)
            self.log.configure(state=tk.DISABLED)

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
            port_name = self.port_var.get()
            try:
                requested_baud = int(self.baud_var.get())
            except ValueError:
                self.status_var.set("Invalid baud rate")
                self.log_notice("ERROR  Baud rate must be a decimal number", "ERROR")
                return
            if not 0 < requested_baud <= 4_000_000:
                self.status_var.set("Baud must be between 1 and 4000000")
                self.log_notice("ERROR  Baud must be between 1 and 4000000", "ERROR")
                return
            simulated_baud = False
            try:
                self.serial_port = serial.Serial(port_name, requested_baud, timeout=0.1)
            except (ValueError, OSError, serial.SerialException) as error:
                if port_name.startswith(("/dev/ttys", "/dev/pts/")):
                    try:
                        self.serial_port = serial.Serial(port_name, 115200, timeout=0.1)
                        simulated_baud = True
                    except (OSError, serial.SerialException):
                        self.status_var.set(str(error))
                        self.log_notice(f"ERROR  {error}", "ERROR")
                        return
                else:
                    self.status_var.set(str(error))
                    self.log_notice(f"ERROR  {error}", "ERROR")
                    return
            self.stop_reader.clear()
            self.reader_thread = threading.Thread(target=self.read_serial, daemon=True)
            self.reader_thread.start()
            self.connect_button.configure(text="Disconnect")
            self.rx_mode_toggle.configure(state=tk.NORMAL)
            suffix = f" (simulating {requested_baud:,} baud on PTY)" if simulated_baud else ""
            self.status_var.set(f"Connected to {self.serial_port.port}{suffix}")
            self.log_notice(f"Connected to {self.serial_port.port} at {requested_baud} baud")
            self.write_frame(build_rx_mode_command(RX_MODE_QUERY), "RX mode query")

        def disconnect(self) -> None:
            """Stop the reader, close the serial device, and reset connection UI."""
            self.stop_reader.set()
            port = self.serial_port
            self.serial_port = None
            if port is not None:
                port_name = port.port
                port.close()
                self.log_notice(f"Disconnected from {port_name}")
            self.connect_button.configure(text="Connect")
            self.rx_mode_toggle.configure(state=tk.DISABLED)
            self.status_var.set("Disconnected")

        def read_serial(self) -> None:
            """Background worker that forwards serial reads to the Tk-safe queue."""
            while not self.stop_reader.is_set():
                port = self.serial_port
                if port is None:
                    return
                try:
                    data = port.read(1)
                    waiting = min(port.in_waiting, 16 * 1024 - len(data))
                    if waiting:
                        data += port.read(waiting)
                    if data:
                        self.rx_queue.put(("data", data))
                except serial.SerialException as error:
                    self.rx_queue.put(("error", error))
                    return

        def drain_reader_queue(self) -> None:
            """Process queued receive, error, and transfer-progress notifications."""
            try:
                while True:
                    kind, value = self.rx_queue.get_nowait()
                    if kind == "error":
                        self.log_notice(f"ERROR  {value}", "ERROR")
                        self.disconnect()
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
                        incoming = value if isinstance(value, bytes) else b""
                        byte_irq_active = self.receive_byte_irq_active
                        if byte_irq_active:
                            self.irq_absolute_count += len(incoming)
                        completed = self.parser.feed(incoming)
                        self.irq_count_var.set(f"IRQ calls: {self.irq_absolute_count}")
                        for frame in completed:
                            if frame.crc_valid and frame.rx_mode is not None:
                                self.receive_byte_irq_active = (
                                    frame.rx_mode == RX_MODE_BYTE_IRQ)
                                self.byte_irq_var.set(self.receive_byte_irq_active)
                            crc_status = "CRC OK" if frame.crc_valid else (
                                f"CRC BAD (got {frame.received_crc:04X}, expected {frame.computed_crc:04X})"
                            )
                            flags = frame.test_transfer_flags
                            irq_detail = ""
                            if flags is not None and flags & 0x01:
                                self.test_irq_active = True
                                self.test_irq_calls = 0
                                self.test_log_next_progress = 256 * 1024
                            if flags is not None and self.test_irq_active and byte_irq_active:
                                self.test_irq_calls += len(frame.h4_packet) + 2
                            if flags is not None and self.test_irq_active and flags & 0x02:
                                transmission_irqs = self.test_irq_calls
                                self.test_irq_active = False
                                irq_detail = (
                                    f", IRQ complete: {transmission_irqs} calls; "
                                    f"app total={self.irq_absolute_count}")
                            elif flags is None:
                                transmission_irqs = len(frame.h4_packet) + 2 if byte_irq_active else 0
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
                    elif kind == "tx_progress":
                        sent, total = value if isinstance(value, tuple) else (0, 0)
                        self.transfer_progress.set(sent / total if total else 0.0)
                        self.transfer_status_var.set(f"{sent} / {total} bytes")
                        self.log_notice(f"TEST  Sent {sent}/{total} bytes ({sent * 100 // total}%)")
                    elif kind == "tx_done":
                        total = int(value)
                        self.transfer_progress.set(1.0)
                        self.transfer_status_var.set(f"{total} / {total} bytes")
                        self.log_notice(f"TEST  All {total} bytes sent; waiting for server verification")
            except queue.Empty:
                pass
            self.root.after(40, self.drain_reader_queue)

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
                self.log_line("TX", frame[:-2], f"[CRC {frame[-2:].hex().upper()}] {detail}")
            except serial.SerialException as error:
                self.log_notice(f"ERROR  Serial write failed: {error}", "ERROR")
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
            self.transfer_progress.set(0.0)
            self.transfer_status_var.set(f"0 / {total_size} bytes")
            self.log_notice(f"TEST  Sending {total_size} bytes (id={identifier})")

            def send_worker() -> None:
                """Write generated test frames and post progress to the UI queue."""
                sent = 0
                next_progress = 256 * 1024
                batch = bytearray()
                try:
                    for frame in build_test_transfer_frames(total_size, identifier):
                        port = self.serial_port
                        if port is None:
                            return
                        batch.extend(frame)
                        chunk_size = int.from_bytes(frame[21:23], "little")
                        sent += chunk_size
                        if len(batch) >= 64 * 1024 or sent == total_size:
                            port.write(batch)
                            batch.clear()
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
            """Send the toggle state and wait for the server's authoritative event."""
            if self.serial_port is None:
                return
            mode = RX_MODE_BYTE_IRQ if self.byte_irq_var.get() else RX_MODE_DMA
            self.write_frame(build_rx_mode_command(mode), "RX mode request")

        def close(self) -> None:
            """Release the serial resource and destroy the Tk root window."""
            self.disconnect()
            self.root.destroy()

    root = tk.Tk()
    preferred_family = ("Segoe UI" if sys.platform == "win32" else
                        "Helvetica Neue" if sys.platform == "darwin" else
                        "DejaVu Sans")
    for named_font in ("TkDefaultFont", "TkTextFont", "TkMenuFont",
                       "TkCaptionFont", "TkSmallCaptionFont"):
        tkfont.nametofont(named_font).configure(family=preferred_family, size=11)
    BleUartApp(root)
    root.mainloop()


if __name__ == "__main__":
    run_gui()
