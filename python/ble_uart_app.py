#!/usr/bin/env python3
"""@file
@brief Tk desktop UART client UI for the BLE HCI transport.

Protocol construction and parsing live in `ble_protocol`; this module owns only
the application controller, background serial reader, and widgets.
"""

from __future__ import annotations

import queue
import threading

from ble_protocol import (
    COMMAND_PRESETS,
    H4CrcStreamParser,
    add_crc,
    build_hci_command,
    build_test_transfer_frames,
    parse_hex,
)
from serial_ports import available_ports


def run_gui() -> None:
    """Create the Tk application and run its main event loop.

    @throws SystemExit If pyserial is not installed.
    """
    try:
        import serial
    except ImportError as error:
        raise SystemExit("pyserial is required. Run: python3 -m pip install -r python/requirements.txt") from error

    import tkinter as tk
    from tkinter import messagebox, scrolledtext, ttk

    class BleUartApp:
        """Coordinates Tk widgets, serial I/O, parsing, and test transfers."""

        def __init__(self, root: tk.Tk) -> None:
            """Initialize application state and construct the complete window.

            @param root Tk root window owned by the caller.
            """
            self.root = root
            self.root.title("BLE UART HCI Client")
            self.root.geometry("880x650")
            self.serial_port: serial.Serial | None = None
            self.reader_thread: threading.Thread | None = None
            self.stop_reader = threading.Event()
            self.rx_queue: queue.Queue[tuple[str, object]] = queue.Queue()
            self.parser = H4CrcStreamParser()
            self.test_identifier = 0

            self.port_var = tk.StringVar()
            self.baud_var = tk.StringVar(value="4000000")
            self.status_var = tk.StringVar(value="Disconnected")
            self.preset_var = tk.StringVar(value=next(iter(COMMAND_PRESETS)))
            self.opcode_var = tk.StringVar(value="0C03")
            self.parameters_var = tk.StringVar()
            self.raw_var = tk.StringVar(value="01 03 0C 00")

            self._build_ui(ttk, scrolledtext)
            self.refresh_ports()
            self.select_preset()
            self.root.after(40, self.drain_reader_queue)
            self.root.protocol("WM_DELETE_WINDOW", self.close)

        def _build_ui(self, ttk_module: object, scrolledtext_module: object) -> None:
            """Create and arrange all connection, command, transfer, and log widgets."""
            del ttk_module, scrolledtext_module
            container = ttk.Frame(self.root, padding=16)
            container.pack(fill=tk.BOTH, expand=True)

            connection = ttk.LabelFrame(container, text="Serial connection", padding=10)
            connection.pack(fill=tk.X)
            ttk.Label(connection, text="Port").grid(row=0, column=0, sticky=tk.W)
            self.port_combo = ttk.Combobox(connection, textvariable=self.port_var, width=42)
            self.port_combo.grid(row=0, column=1, padx=8, sticky=tk.EW)
            ttk.Button(connection, text="Refresh", command=self.refresh_ports).grid(row=0, column=2)
            ttk.Label(connection, text="Baud").grid(row=0, column=3, padx=(16, 0))
            ttk.Combobox(connection, textvariable=self.baud_var, width=10,
                         values=("9600", "57600", "115200", "460800", "921600",
                                 "1000000", "2000000", "3000000", "4000000")).grid(row=0, column=4, padx=8)
            self.connect_button = ttk.Button(connection, text="Connect", command=self.toggle_connection)
            self.connect_button.grid(row=0, column=5)
            ttk.Label(connection, textvariable=self.status_var).grid(row=1, column=1, sticky=tk.W, pady=(8, 0))
            connection.columnconfigure(1, weight=1)

            command = ttk.LabelFrame(container, text="Send HCI command", padding=10)
            command.pack(fill=tk.X, pady=(12, 0))
            ttk.Label(command, text="Preset").grid(row=0, column=0, sticky=tk.W)
            preset = ttk.Combobox(command, textvariable=self.preset_var,
                                  values=tuple(COMMAND_PRESETS), state="readonly", width=28)
            preset.grid(row=0, column=1, padx=8, sticky=tk.W)
            preset.bind("<<ComboboxSelected>>", lambda _event: self.select_preset())
            ttk.Label(command, text="Opcode").grid(row=0, column=2, padx=(16, 0))
            ttk.Entry(command, textvariable=self.opcode_var, width=10).grid(row=0, column=3, padx=8)
            ttk.Label(command, text="Parameters (hex)").grid(row=1, column=0, sticky=tk.W, pady=(10, 0))
            ttk.Entry(command, textvariable=self.parameters_var).grid(row=1, column=1, columnspan=3,
                                                                       padx=8, pady=(10, 0), sticky=tk.EW)
            ttk.Button(command, text="Send command", command=self.send_command).grid(row=0, column=4,
                                                                                       rowspan=2, padx=(12, 0))
            command.columnconfigure(1, weight=1)

            raw = ttk.LabelFrame(container, text="Send raw H4 packet", padding=10)
            raw.pack(fill=tk.X, pady=(12, 0))
            ttk.Entry(raw, textvariable=self.raw_var).pack(side=tk.LEFT, fill=tk.X, expand=True)
            ttk.Button(raw, text="Append CRC + send", command=self.send_raw).pack(side=tk.LEFT, padx=(10, 0))

            bulk = ttk.LabelFrame(container, text="Bulk test messages", padding=10)
            bulk.pack(fill=tk.X, pady=(12, 0))
            ttk.Button(bulk, text="Send 128 bytes",
                       command=lambda: self.start_test_transfer(128)).pack(side=tk.LEFT)
            ttk.Button(bulk, text="Send 2 MiB",
                       command=lambda: self.start_test_transfer(2 * 1024 * 1024)).pack(side=tk.LEFT, padx=(10, 0))
            self.transfer_status = ttk.Label(bulk, text="Ready")
            self.transfer_status.pack(side=tk.LEFT, padx=(16, 0))

            ttk.Label(container, text="UART traffic").pack(anchor=tk.W, pady=(12, 4))
            self.log = scrolledtext.ScrolledText(container, height=20, state=tk.DISABLED,
                                                  font=("Menlo", 11))
            self.log.pack(fill=tk.BOTH, expand=True)

        def log_line(self, direction: str, data: bytes, detail: str = "") -> None:
            """Append one formatted TX or RX record to the traffic view.

            @param direction Short direction label such as `TX` or `RX`.
            @param data Wire bytes to display.
            @param detail Optional protocol description.
            """
            suffix = f"  {detail}" if detail else ""
            self.log.configure(state=tk.NORMAL)
            self.log.insert(tk.END, f"{direction:<4} {data.hex(' ').upper()}{suffix}\n")
            self.log.see(tk.END)
            self.log.configure(state=tk.DISABLED)

        def refresh_ports(self) -> None:
            """Refresh the serial-device picker while preserving a valid selection."""
            ports = list(available_ports())
            devices = [device for device, _description in ports]
            self.port_combo["values"] = devices
            if devices and self.port_var.get() not in devices:
                self.port_var.set(devices[0])
            self.status_var.set(f"{len(devices)} serial port(s) found" if devices else "No serial ports found")

        def toggle_connection(self) -> None:
            """Connect the selected port or disconnect the active port.

            Pseudo-terminals fall back to a simulated 115,200 setting when their
            driver rejects the user-selected logical baud rate.
            """
            if self.serial_port is not None:
                self.disconnect()
                return
            port_name = self.port_var.get()
            requested_baud = int(self.baud_var.get())
            if not 0 < requested_baud <= 4_000_000:
                messagebox.showerror("Invalid baud rate", "Baud must be between 1 and 4,000,000.")
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
                        messagebox.showerror("Connection failed", str(error))
                        return
                else:
                    messagebox.showerror("Connection failed", str(error))
                    return
            self.stop_reader.clear()
            self.reader_thread = threading.Thread(target=self.read_serial, daemon=True)
            self.reader_thread.start()
            self.connect_button.configure(text="Disconnect")
            suffix = f" (simulating {requested_baud:,} baud on PTY)" if simulated_baud else ""
            self.status_var.set(f"Connected to {self.serial_port.port}{suffix}")

        def disconnect(self) -> None:
            """Stop the reader, close the serial device, and reset connection UI."""
            self.stop_reader.set()
            port = self.serial_port
            self.serial_port = None
            if port is not None:
                port.close()
            self.connect_button.configure(text="Connect")
            self.status_var.set("Disconnected")

        def read_serial(self) -> None:
            """Background worker that forwards serial reads to the Tk-safe queue."""
            while not self.stop_reader.is_set():
                port = self.serial_port
                if port is None:
                    return
                try:
                    data = port.read(256)
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
                        messagebox.showerror("Serial error", str(value))
                        self.disconnect()
                    elif kind == "data":
                        for frame in self.parser.feed(value if isinstance(value, bytes) else b""):
                            crc_status = "CRC OK" if frame.crc_valid else (
                                f"CRC BAD (got {frame.received_crc:04X}, expected {frame.computed_crc:04X})"
                            )
                            self.log_line("RX", frame.h4_packet + frame.received_crc.to_bytes(2, "big"),
                                          f"{frame.summary}; {crc_status}")
                    elif kind == "tx_progress":
                        sent, total = value if isinstance(value, tuple) else (0, 0)
                        self.transfer_status.configure(text=f"Sending {sent:,} / {total:,} bytes")
                    elif kind == "tx_done":
                        self.transfer_status.configure(text=f"Sent {int(value):,} bytes; awaiting verification")
            except queue.Empty:
                pass
            self.root.after(40, self.drain_reader_queue)

        def write_frame(self, frame: bytes, detail: str) -> None:
            """Synchronously write one complete frame and log it.

            @param frame H4 packet and CRC trailer.
            @param detail Human-readable protocol description.
            """
            if self.serial_port is None:
                messagebox.showwarning("Not connected", "Connect to a serial port first.")
                return
            try:
                self.serial_port.write(frame)
                self.serial_port.flush()
                self.log_line("TX", frame, detail)
            except serial.SerialException as error:
                messagebox.showerror("Send failed", str(error))
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
                messagebox.showerror("Invalid command", str(error))
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
                messagebox.showerror("Invalid packet", str(error))
                return
            self.write_frame(frame, f"raw H4; CRC {frame[-2:].hex().upper()}")

        def start_test_transfer(self, total_size: int) -> None:
            """Start a background deterministic test transfer.

            @param total_size Logical number of generated payload bytes.
            """
            if self.serial_port is None:
                messagebox.showwarning("Not connected", "Connect to a serial port first.")
                return
            self.test_identifier = (self.test_identifier + 1) & 0xFFFF or 1
            identifier = self.test_identifier

            def send_worker() -> None:
                """Write generated test frames and post progress to the UI queue."""
                sent = 0
                next_progress = 256 * 1024
                try:
                    for frame in build_test_transfer_frames(total_size, identifier):
                        port = self.serial_port
                        if port is None:
                            return
                        port.write(frame)
                        chunk_size = int.from_bytes(frame[21:23], "little")
                        sent += chunk_size
                        if sent >= next_progress:
                            self.rx_queue.put(("tx_progress", (sent, total_size)))
                            next_progress += 256 * 1024
                    port = self.serial_port
                    if port is not None:
                        port.flush()
                    self.rx_queue.put(("tx_done", total_size))
                except serial.SerialException as error:
                    self.rx_queue.put(("error", error))

            self.transfer_status.configure(text=f"Starting {total_size:,}-byte transfer")
            threading.Thread(target=send_worker, daemon=True).start()

        def close(self) -> None:
            """Release the serial resource and destroy the Tk root window."""
            self.disconnect()
            self.root.destroy()

    root = tk.Tk()
    BleUartApp(root)
    root.mainloop()


if __name__ == "__main__":
    run_gui()
