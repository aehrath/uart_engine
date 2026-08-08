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
from typing import Callable

from ble_protocol import (
    COMMAND_PRESETS,
    H4CrcStreamParser,
    TEST_TRANSFER_SIZES,
    add_crc,
    build_hci_command,
    build_test_transfer_frames,
    parse_hex,
)
from serial_ports import available_ports

RING_BUFFER_SIZES: tuple[tuple[str, int], ...] = (
    ("256 B", 256), ("1 KiB", 1024), ("4 KiB", 4096),
    ("16 KiB", 16 * 1024), ("64 KiB", 64 * 1024),
)
DMA_BUFFER_SIZES: tuple[tuple[str, int], ...] = (
    ("32 B", 32), ("64 B", 64), ("128 B", 128), ("256 B", 256),
    ("512 B", 512), ("1 KiB", 1024), ("4 KiB", 4096),
)


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

    class RoundedActionButton(tk.Canvas):
        """Bright keyboard-accessible rounded action button for the dark dashboard."""

        def __init__(self, parent: tk.Misc, text: str, command: object,
                     width: int = 190, height: int = 42) -> None:
            super().__init__(parent, width=width, height=height,
                             background="#141C2E", highlightthickness=0,
                             borderwidth=0, takefocus=True)
            self._text = text
            self._command = command
            self._state = tk.NORMAL
            self._hovered = False
            self._width = width
            self._height = height
            self.bind("<Enter>", lambda _event: self._set_hover(True))
            self.bind("<Leave>", lambda _event: self._set_hover(False))
            self.bind("<ButtonRelease-1>", self._activate)
            self.bind("<Return>", self._activate)
            self.bind("<space>", self._activate)
            self._draw()

        def _set_hover(self, hovered: bool) -> None:
            self._hovered = hovered
            self._draw()

        def _draw(self) -> None:
            self.delete("all")
            enabled = self._state != tk.DISABLED
            fill = "#67E8F9" if enabled and self._hovered else (
                "#2DD4BF" if enabled else "#273143")
            radius = 8
            self.create_rectangle(radius, 0, self._width - radius, self._height,
                                  fill=fill, outline=fill)
            self.create_rectangle(0, radius, self._width, self._height - radius,
                                  fill=fill, outline=fill)
            self.create_oval(0, 0, radius * 2, radius * 2, fill=fill, outline=fill)
            self.create_oval(self._width - radius * 2, 0, self._width, radius * 2,
                             fill=fill, outline=fill)
            self.create_oval(0, self._height - radius * 2, radius * 2, self._height,
                             fill=fill, outline=fill)
            self.create_oval(self._width - radius * 2, self._height - radius * 2,
                             self._width, self._height, fill=fill, outline=fill)
            self.create_text(self._width / 2, self._height / 2,
                             text=self._text,
                             fill="#07161C" if enabled else "#5E6B7F",
                             font=(preferred_family, -16, "bold"))

        def _activate(self, _event: object = None) -> None:
            if self._state != tk.DISABLED and callable(self._command):
                self._command()

        def configure(self, cnf: object = None, **kwargs: object) -> object:
            if "text" in kwargs:
                self._text = str(kwargs.pop("text"))
            if "state" in kwargs:
                self._state = str(kwargs.pop("state"))
            self._draw()
            return super().configure(cnf, **kwargs)

        config = configure

    class RoundedToggle(tk.Canvas):
        """Shared pill toggle used instead of a square platform checkbox."""

        def __init__(self, parent: tk.Misc, text: str, variable: tk.BooleanVar,
                     command: Callable[[], None], width: int = 120,
                     height: int = 30) -> None:
            super().__init__(parent, width=width, height=height,
                             background="#141C2E", highlightthickness=0,
                             borderwidth=0, cursor="hand2")
            self._text = text
            self._variable = variable
            self._command = command
            self._state = tk.NORMAL
            self._width = width
            self._height = height
            self.bind("<ButtonRelease-1>", self._activate)
            self._draw()

        def _draw(self) -> None:
            self.delete("all")
            enabled = self._state != tk.DISABLED
            selected = self._variable.get()
            track = "#2DD4BF" if selected and enabled else "#253451"
            knob = "#07161C" if selected and enabled else "#8F9EB5"
            self.create_rectangle(10, 5, 28, 25, fill=track, outline=track)
            self.create_oval(0, 5, 20, 25, fill=track, outline=track)
            self.create_oval(18, 5, 38, 25, fill=track, outline=track)
            knob_x = 19 if selected else 1
            self.create_oval(knob_x, 7, knob_x + 16, 23, fill=knob, outline=knob)
            self.create_text(46, self._height / 2, text=self._text, anchor=tk.W,
                             fill="#E8EFF9" if enabled else "#5E6B7F",
                             font=(preferred_family, -15))

        def _activate(self, _event: object = None) -> None:
            if self._state == tk.DISABLED:
                return
            self._variable.set(not self._variable.get())
            self._draw()
            self._command()

        def configure(self, cnf: object = None, **kwargs: object) -> object:
            if "state" in kwargs:
                self._state = str(kwargs.pop("state"))
                self._draw()
            return super().configure(cnf, **kwargs)

        config = configure

    class RoundedProgress(tk.Canvas):
        """Rounded progress indicator sharing the field corner geometry."""

        def __init__(self, parent: tk.Misc, variable: tk.DoubleVar) -> None:
            super().__init__(parent, background="#141C2E", highlightthickness=0,
                             borderwidth=0)
            self._variable = variable
            self._variable.trace_add("write", lambda *_args: self._draw())
            self.bind("<Configure>", lambda _event: self._draw())

        def _draw(self) -> None:
            self.delete("all")
            width = max(1, self.winfo_width())
            height = max(1, self.winfo_height())
            progress = min(1.0, max(0.0, self._variable.get()))
            radius = min(8, height // 2)

            def pill(x1: float, color: str) -> None:
                if x1 <= 0:
                    return
                local_radius = min(radius, int(x1 / 2))
                self.create_rectangle(local_radius, 0, max(local_radius, x1-local_radius),
                                      height, fill=color, outline=color)
                self.create_oval(0, 0, local_radius*2, height,
                                 fill=color, outline=color)
                self.create_oval(max(0, x1-local_radius*2), 0, x1, height,
                                 fill=color, outline=color)

            pill(width, "#232D41")
            pill(width * progress, "#4C94FF")

    class RoundedSurface(tk.Frame):
        """Canvas-backed rounded container matching the native dashboard panels."""

        def __init__(self, parent: tk.Misc, *, fill: str, border: str,
                     style: str, radius: int,
                     content_padding: tuple[int, int]) -> None:
            super().__init__(parent, background="#0A0F1C", borderwidth=0,
                             highlightthickness=0)
            self._fill = fill
            self._border = border
            self._radius = radius
            self._canvas = tk.Canvas(self, background="#0A0F1C",
                                     borderwidth=0, highlightthickness=0)
            self._canvas.place(x=0, y=0, relwidth=1, relheight=1)
            self.content = ttk.Frame(self, padding=content_padding, style=style)
            self.content.pack(fill=tk.BOTH, expand=True, padx=1,
                              pady=max(1, radius // 2))
            self.bind("<Configure>", self._redraw)

        def _rounded_rectangle(self, inset: int, radius: int, color: str) -> None:
            width = max(1, self.winfo_width() - 1 - inset * 2)
            height = max(1, self.winfo_height() - 1 - inset * 2)
            x0, y0 = inset, inset
            x1, y1 = inset + width, inset + height
            radius = max(1, min(radius, width // 2, height // 2))
            self._canvas.create_rectangle(x0 + radius, y0, x1 - radius, y1,
                                          fill=color, outline=color)
            self._canvas.create_rectangle(x0, y0 + radius, x1, y1 - radius,
                                          fill=color, outline=color)
            self._canvas.create_oval(x0, y0, x0 + radius * 2, y0 + radius * 2,
                                     fill=color, outline=color)
            self._canvas.create_oval(x1 - radius * 2, y0, x1, y0 + radius * 2,
                                     fill=color, outline=color)
            self._canvas.create_oval(x0, y1 - radius * 2, x0 + radius * 2, y1,
                                     fill=color, outline=color)
            self._canvas.create_oval(x1 - radius * 2, y1 - radius * 2, x1, y1,
                                     fill=color, outline=color)

        def _redraw(self, _event: object = None) -> None:
            self._canvas.delete("all")
            self._rounded_rectangle(0, self._radius, self._border)
            self._rounded_rectangle(1, max(1, self._radius - 1), self._fill)

    class RoundedEntry(tk.Frame):
        """Single-line left-aligned field with the shared rounded surface."""

        def __init__(self, parent: tk.Misc, variable: tk.StringVar,
                     *, readonly: bool = False) -> None:
            super().__init__(parent, background="#141C2E", borderwidth=0,
                             highlightthickness=0)
            self._canvas = tk.Canvas(self, background="#141C2E", borderwidth=0,
                                     highlightthickness=0)
            self._canvas.place(x=0, y=0, relwidth=1, relheight=1)
            self.entry = tk.Entry(
                self, textvariable=variable, background="#1B253B",
                foreground="#E8EFF9", insertbackground="#E8EFF9",
                readonlybackground="#1B253B", relief=tk.FLAT, borderwidth=0,
                highlightthickness=0, font=(preferred_family, -15), justify=tk.LEFT)
            self.entry.place(x=10, y=2, relwidth=1, width=-20,
                             relheight=1, height=-4)
            if readonly:
                self.entry.configure(state="readonly")
            self.entry.bind("<FocusIn>", lambda _event: self._draw(True))
            self.entry.bind("<FocusOut>", lambda _event: self._draw(False))
            self.bind("<Configure>", lambda _event: self._draw(False))

        def _draw(self, focused: bool) -> None:
            self._canvas.delete("all")
            width = max(2, self.winfo_width() - 1)
            height = max(2, self.winfo_height() - 1)
            border = "#2DD4BF" if focused else "#405574"
            radius = min(8, height // 2)
            for inset, color in ((0, border), (1, "#1B253B")):
                x0 = y0 = inset
                x1, y1 = width - inset, height - inset
                r = max(1, radius - inset)
                self._canvas.create_rectangle(x0 + r, y0, x1 - r, y1,
                                              fill=color, outline=color)
                self._canvas.create_rectangle(x0, y0 + r, x1, y1 - r,
                                              fill=color, outline=color)
                self._canvas.create_oval(x0, y0, x0 + 2 * r, y0 + 2 * r,
                                         fill=color, outline=color)
                self._canvas.create_oval(x1 - 2 * r, y0, x1, y0 + 2 * r,
                                         fill=color, outline=color)
                self._canvas.create_oval(x0, y1 - 2 * r, x0 + 2 * r, y1,
                                         fill=color, outline=color)
                self._canvas.create_oval(x1 - 2 * r, y1 - 2 * r, x1, y1,
                                         fill=color, outline=color)

    class RoundedCombo(tk.Frame):
        """Rounded dropdown with centered selections and a matching popup."""

        def __init__(self, parent: tk.Misc, variable: tk.StringVar,
                     values: tuple[str, ...] = (), *, editable: bool = False) -> None:
            super().__init__(parent, background="#141C2E", borderwidth=0,
                             highlightthickness=0)
            self.variable = variable
            self.values = tuple(values)
            self.editable = editable
            self.popup: tk.Toplevel | None = None
            self._canvas = tk.Canvas(self, background="#141C2E", borderwidth=0,
                                     highlightthickness=0, cursor="hand2")
            self._canvas.place(x=0, y=0, relwidth=1, relheight=1)
            self.entry = tk.Entry(
                self, textvariable=variable, background="#1B253B",
                foreground="#E8EFF9", insertbackground="#E8EFF9",
                readonlybackground="#1B253B", relief=tk.FLAT, borderwidth=0,
                highlightthickness=0, font=(preferred_family, -15),
                justify=tk.LEFT if editable else tk.CENTER)
            self.entry.place(x=10, y=2, relwidth=1, width=-42,
                             relheight=1, height=-4)
            if not editable:
                self.entry.configure(state="readonly", cursor="hand2")
                self.entry.bind("<Button-1>", self._open_popup)
            self.entry.bind("<FocusIn>", lambda _event: self._draw(True))
            self.entry.bind("<FocusOut>", lambda _event: self._draw(False))
            self._canvas.bind("<Button-1>", self._open_popup)
            self.bind("<Configure>", lambda _event: self._draw(False))

        def __setitem__(self, key: str, value: object) -> None:
            if key != "values":
                raise KeyError(key)
            self.values = tuple(str(item) for item in value)  # type: ignore[arg-type]

        def _draw(self, focused: bool) -> None:
            self._canvas.delete("all")
            width = max(2, self.winfo_width() - 1)
            height = max(2, self.winfo_height() - 1)
            border = "#2DD4BF" if focused or self.popup is not None else "#405574"
            radius = min(8, height // 2)
            for inset, color in ((0, border), (1, "#1B253B")):
                x0 = y0 = inset
                x1, y1 = width - inset, height - inset
                r = max(1, radius - inset)
                self._canvas.create_rectangle(x0 + r, y0, x1 - r, y1,
                                              fill=color, outline=color)
                self._canvas.create_rectangle(x0, y0 + r, x1, y1 - r,
                                              fill=color, outline=color)
                for oval in ((x0, y0, x0 + 2*r, y0 + 2*r),
                             (x1 - 2*r, y0, x1, y0 + 2*r),
                             (x0, y1 - 2*r, x0 + 2*r, y1),
                             (x1 - 2*r, y1 - 2*r, x1, y1)):
                    self._canvas.create_oval(*oval, fill=color, outline=color)
            arrow_x, arrow_y = width - 17, height // 2
            self._canvas.create_polygon(arrow_x - 5, arrow_y - 3,
                                        arrow_x + 5, arrow_y - 3,
                                        arrow_x, arrow_y + 4,
                                        fill="#E8EFF9", outline="")

        def _open_popup(self, _event: object = None) -> str:
            if not self.values or self.popup is not None:
                return "break"
            popup = tk.Toplevel(self)
            self.popup = popup
            popup.overrideredirect(True)
            popup.configure(background="#0E1626")
            row_height = 32
            popup_height = row_height * len(self.values) + 4
            popup.update_idletasks()
            x = self.winfo_rootx()
            y = self.winfo_rooty() + self.winfo_height() + 4
            if y + popup_height > self.winfo_screenheight() - 8:
                y = self.winfo_rooty() - popup_height - 4
            popup.geometry(f"{self.winfo_width()}x{popup_height}+{x}+{y}")
            canvas = tk.Canvas(popup, background="#0E1626", borderwidth=0,
                               highlightthickness=0, cursor="hand2")
            canvas.pack(fill=tk.BOTH, expand=True)

            def draw_popup(hovered: int = -1) -> None:
                canvas.delete("all")
                for index, item in enumerate(self.values):
                    y0 = 2 + index * row_height
                    selected = item == self.variable.get()
                    fill = "#67E8F9" if index == hovered else (
                        "#4C94FF" if selected else "#141C2E")
                    color = "#07161C" if index == hovered else "#F1F6FF"
                    x0, x1 = 2, self.winfo_width() - 2
                    y1, radius = y0 + row_height - 2, 6
                    canvas.create_rectangle(x0 + radius, y0, x1 - radius, y1,
                                            fill=fill, outline=fill)
                    canvas.create_rectangle(x0, y0 + radius, x1, y1 - radius,
                                            fill=fill, outline=fill)
                    for oval in ((x0, y0, x0 + 2*radius, y0 + 2*radius),
                                 (x1 - 2*radius, y0, x1, y0 + 2*radius),
                                 (x0, y1 - 2*radius, x0 + 2*radius, y1),
                                 (x1 - 2*radius, y1 - 2*radius, x1, y1)):
                        canvas.create_oval(*oval, fill=fill, outline=fill)
                    canvas.create_text(self.winfo_width() / 2,
                                       y0 + (row_height - 2) / 2,
                                       text=item, fill=color,
                                       font=(preferred_family, -15))

            def motion(event: tk.Event) -> None:
                draw_popup(min(len(self.values) - 1,
                               max(0, (event.y - 2) // row_height)))

            def select(event: tk.Event) -> None:
                index = min(len(self.values) - 1, max(0, (event.y - 2) // row_height))
                self.variable.set(self.values[index])
                self.event_generate("<<ComboboxSelected>>")
                self._close_popup()

            canvas.bind("<Motion>", motion)
            canvas.bind("<Leave>", lambda _event: draw_popup())
            canvas.bind("<ButtonRelease-1>", select)
            popup.bind("<Escape>", lambda _event: self._close_popup())
            popup.bind("<FocusOut>", lambda _event: self._close_popup())
            draw_popup()
            popup.focus_force()
            self._draw(True)
            return "break"

        def _close_popup(self) -> None:
            popup, self.popup = self.popup, None
            if popup is not None and popup.winfo_exists():
                popup.destroy()
            self._draw(self.entry.focus_get() == self.entry)

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
            self.rx_queue_lock = threading.Lock()
            self.rx_queued_bytes = 0
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
            self.ring_buffer_var = tk.StringVar(value="16 KiB")
            self.dma_buffer_var = tk.StringVar(value="256 B")
            self.ring_buffer_size = 16 * 1024
            self.dma_buffer_size = 256
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
            self.root.after(40, self.drain_reader_queue)
            self.root.protocol("WM_DELETE_WINDOW", self.close)

        def _configure_styles(self) -> None:
            """Configure the shared dark instrument-panel visual system."""
            style = ttk.Style(self.root)
            if "clam" in style.theme_names():
                style.theme_use("clam")
            body = preferred_family
            style.configure("App.TFrame", background="#0A0F1C")
            style.configure("Panel.TFrame", background="#141C2E", borderwidth=0,
                            relief=tk.FLAT)
            style.configure("Card.TFrame", background="#1B253B")
            style.configure("App.TLabel", background="#141C2E", foreground="#E8EFF9",
                            font=(body, -15))
            style.configure("Muted.TLabel", background="#141C2E", foreground="#8F9EB5",
                            font=(body, -12))
            style.configure("Section.TLabel", background="#141C2E", foreground="#8F9EB5",
                            font=(body, -14, "bold"))
            style.configure("CardLabel.TLabel", background="#1B253B", foreground="#8F9EB5",
                            font=(body, -12, "bold"))
            style.configure("CardValue.TLabel", background="#1B253B", foreground="#E8EFF9",
                            font=(body, -22, "bold"))
            style.configure("Status.TLabel", background="#1B253B", foreground="#E8EFF9",
                            borderwidth=0, relief=tk.FLAT, padding=(10, 6), font=(body, -15))
            style.configure("TButton", background="#253451", foreground="#F4F8FF",
                            bordercolor="#405574", padding=(12, 9), font=(body, -16, "bold"))
            style.map("TButton", background=[("active", "#32486F"), ("pressed", "#3B5B8E")])
            style.configure("Accent.TButton", background="#356FD6", foreground="#FFFFFF",
                            bordercolor="#4C94FF")
            style.map("Accent.TButton", background=[("active", "#4385ED"),
                                                     ("pressed", "#2F62BE")])
            style.configure("TEntry", fieldbackground="#1B253B", foreground="#E8EFF9",
                            insertcolor="#E8EFF9", bordercolor="#405574",
                            lightcolor="#405574", darkcolor="#405574",
                            focuscolor="#2DD4BF", borderwidth=1, relief=tk.FLAT,
                            padding=7, font=(body, -15))
            style.map("TEntry",
                      bordercolor=[("focus", "#2DD4BF"), ("active", "#405574")],
                      lightcolor=[("focus", "#2DD4BF"), ("active", "#405574")],
                      darkcolor=[("focus", "#2DD4BF"), ("active", "#405574")])
            style.configure("TCombobox", fieldbackground="#1B253B", foreground="#E8EFF9",
                            background="#253451", arrowcolor="#8F9EB5",
                            bordercolor="#405574", lightcolor="#405574",
                            darkcolor="#405574", focuscolor="#2DD4BF",
                            borderwidth=1, relief=tk.FLAT, padding=6, font=(body, -15))
            style.map("TCombobox", fieldbackground=[("readonly", "#1B253B")],
                      foreground=[("readonly", "#E8EFF9")],
                      bordercolor=[("focus", "#2DD4BF"), ("active", "#405574")],
                      lightcolor=[("focus", "#2DD4BF"), ("active", "#405574")],
                      darkcolor=[("focus", "#2DD4BF"), ("active", "#405574")])
            self.root.option_add("*TCombobox*Listbox.background", "#0E1626")
            self.root.option_add("*TCombobox*Listbox.foreground", "#F1F6FF")
            self.root.option_add("*TCombobox*Listbox.selectBackground", "#67E8F9")
            self.root.option_add("*TCombobox*Listbox.selectForeground", "#07161C")
            style.configure("TCheckbutton", background="#141C2E", foreground="#E8EFF9",
                            font=(body, -15, "bold"), indicatorcolor="#1B253B",
                            indicatorrelief=tk.FLAT)
            style.map("TCheckbutton", background=[("active", "#141C2E")],
                      indicatorcolor=[("selected", "#2DD4BF")])
            style.configure("Transfer.Horizontal.TProgressbar", troughcolor="#232D41",
                            background="#4C94FF", bordercolor="#232D41",
                            lightcolor="#232D41", darkcolor="#232D41",
                            borderwidth=0, relief=tk.FLAT, thickness=14)
            style.configure("Dark.Vertical.TScrollbar", background="#253451",
                            troughcolor="#070B14", bordercolor="#070B14",
                            lightcolor="#253451", darkcolor="#253451",
                            arrowcolor="#8F9EB5", borderwidth=0, relief=tk.FLAT)

        def _build_ui(self) -> None:
            """Create the shared telemetry-first client dashboard layout."""
            self._configure_styles()
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
            ttk.Label(connection, text="Baud", style="App.TLabel").place(
                x=498, y=3, width=52, height=38)
            RoundedCombo(
                connection, self.baud_var,
                ("9600", "57600", "115200", "460800", "921600",
                 "1000000", "2000000", "3000000", "4000000"),
                editable=True).place(x=554, y=3, width=138, height=38)
            self.connect_button = RoundedActionButton(
                connection, "Connect", self.toggle_connection, width=170, height=38)
            self.connect_button.place(x=712, y=3)
            self.rx_mode_toggle = RoundedToggle(
                connection, "Per-byte IRQ", self.byte_irq_var,
                self.send_rx_mode_request)
            self.rx_mode_toggle.configure(state=tk.DISABLED)
            self.rx_mode_toggle.place(x=914, y=7, width=120, height=30)
            RoundedEntry(connection, self.status_var, readonly=True).place(
                x=0, y=54, width=414, height=28)
            ttk.Label(connection, text="Ring", style="Muted.TLabel").place(
                x=436, y=54, width=36, height=28)
            ring_combo = RoundedCombo(
                connection, self.ring_buffer_var,
                tuple(label for label, _size in RING_BUFFER_SIZES))
            ring_combo.place(x=476, y=54, width=100, height=28)
            ring_combo.bind("<<ComboboxSelected>>", self._configure_receive_buffers)
            ttk.Label(connection, text="DMA", style="Muted.TLabel").place(
                x=596, y=54, width=36, height=28)
            dma_combo = RoundedCombo(
                connection, self.dma_buffer_var,
                tuple(label for label, _size in DMA_BUFFER_SIZES))
            dma_combo.place(x=636, y=54, width=100, height=28)
            dma_combo.bind("<<ComboboxSelected>>", self._configure_receive_buffers)
            ttk.Label(connection, text="Native clipboard shortcuts enabled",
                      style="Muted.TLabel").place(x=2, y=82, height=16)

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
            RoundedCombo(
                bulk, self.bulk_size_var,
                tuple(label for label, _size in TEST_TRANSFER_SIZES)).place(
                    x=348, y=5, width=132, height=42)
            self.send_large_button = RoundedActionButton(
                bulk, "Send selected block", self.send_selected_test_transfer,
                width=218, height=42)
            self.send_large_button.configure(state=tk.DISABLED)
            self.send_large_button.place(x=504, y=5)
            RoundedProgress(bulk, self.transfer_progress).place(
                x=748, y=5, width=284, height=16)
            ttk.Label(bulk, textvariable=self.transfer_status_var, style="Muted.TLabel").place(
                x=748, y=27, width=284, height=20)

            traffic = panel("LIVE UART TRAFFIC", (24, 740, 1072, 196))
            log_frame = tk.Frame(traffic, background="#141C2E",
                                 borderwidth=0, highlightthickness=0)
            log_frame.pack(fill=tk.BOTH, expand=True)
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
            following_tail = self.log.yview()[1] >= 0.999
            self.log.configure(state=tk.NORMAL)
            self.log.insert(tk.END, f"{message}\n", kind)
            self.log_line_count += 1
            if self.log_line_count > 256:
                self.log.delete("1.0", "2.0")
                self.log_line_count -= 1
            if following_tail:
                self.log.see(tk.END)
            self.log.configure(state=tk.DISABLED)

        def _configure_receive_buffers(self, _event: object = None) -> None:
            """Apply the selected local ring capacity and DMA transaction size."""
            self.ring_buffer_size = dict(RING_BUFFER_SIZES)[self.ring_buffer_var.get()]
            self.dma_buffer_size = dict(DMA_BUFFER_SIZES)[self.dma_buffer_var.get()]
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
            with self.rx_queue_lock:
                self.rx_queued_bytes = 0
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
                    with self.rx_queue_lock:
                        available = self.ring_buffer_size - self.rx_queued_bytes
                    if available <= 0:
                        self.stop_reader.wait(0.002)
                        continue
                    capacity = min(self.dma_buffer_size, available)
                    data = port.read(1)
                    waiting = min(port.in_waiting, capacity - len(data))
                    if waiting:
                        data += port.read(waiting)
                    if data:
                        with self.rx_queue_lock:
                            self.rx_queued_bytes += len(data)
                        self.rx_queue.put(("data", data))
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
                        incoming = value if isinstance(value, bytes) else b""
                        with self.rx_queue_lock:
                            self.rx_queued_bytes = max(
                                0, self.rx_queued_bytes - len(incoming))
                        byte_irq_active = self.receive_byte_irq_active
                        self.irq_absolute_count += len(incoming) if byte_irq_active else 1
                        completed = self.parser.feed(incoming)
                        self.irq_count_var.set(f"IRQ calls: {self.irq_absolute_count}")
                        self.irq_metric_var.set(str(self.irq_absolute_count))
                        dma_test_irq_accounted = False
                        for frame in completed:
                            self.rx_packets += 1
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
                                if byte_irq_active:
                                    self.test_irq_calls += len(frame.h4_packet) + 2
                                elif not dma_test_irq_accounted:
                                    self.test_irq_calls += 1
                                    dma_test_irq_accounted = True
                            if flags is not None and self.test_irq_active and flags & 0x02:
                                transmission_irqs = self.test_irq_calls
                                self.test_irq_active = False
                                irq_detail = (
                                    f", IRQ complete: {transmission_irqs} calls; "
                                    f"app total={self.irq_absolute_count}")
                            elif flags is None:
                                transmission_irqs = (len(frame.h4_packet) + 2
                                                     if byte_irq_active else 1)
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
                    elif kind == "tx_packets":
                        self.tx_packets += int(value)
                        self.tx_packets_var.set(str(self.tx_packets))
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
                        if self.serial_port is not None:
                            self.send_small_button.configure(state=tk.NORMAL)
                            self.send_large_button.configure(state=tk.NORMAL)
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
            self.transfer_progress.set(0.0)
            self.transfer_status_var.set(f"0 / {total_size} bytes")
            self.send_small_button.configure(state=tk.DISABLED)
            self.send_large_button.configure(state=tk.DISABLED)
            self.log_notice(f"TEST  Sending {total_size} bytes (id={identifier})")

            def send_worker() -> None:
                """Write generated test frames and post progress to the UI queue."""
                sent = 0
                next_progress = 256 * 1024
                batch = bytearray()
                batch_packets = 0
                try:
                    for frame in build_test_transfer_frames(total_size, identifier):
                        port = self.serial_port
                        if port is None:
                            return
                        batch.extend(frame)
                        batch_packets += 1
                        chunk_size = int.from_bytes(frame[21:23], "little")
                        sent += chunk_size
                        if len(batch) >= 64 * 1024 or sent == total_size:
                            port.write(batch)
                            batch.clear()
                            self.rx_queue.put(("tx_packets", batch_packets))
                            batch_packets = 0
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

        def close(self) -> None:
            """Release the serial resource and destroy the Tk root window."""
            self.disconnect()
            self.root.destroy()

    root = tk.Tk()
    available_families = set(tkfont.families(root))
    preferred_family = "Space Grotesk" if "Space Grotesk" in available_families else (
        "Segoe UI" if sys.platform == "win32" else
        "Avenir Next" if sys.platform == "darwin" else "DejaVu Sans")
    for named_font in ("TkDefaultFont", "TkTextFont", "TkMenuFont",
                       "TkCaptionFont", "TkSmallCaptionFont"):
        tkfont.nametofont(named_font).configure(family=preferred_family, size=-15)
    BleUartApp(root)
    root.mainloop()


if __name__ == "__main__":
    run_gui()
