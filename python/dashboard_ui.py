"""Shared rounded Tk dashboard controls mirroring apps/common/dashboard_ui.

The widgets in this module deliberately share geometry, colors, alignment, and
keyboard behavior with the native Raylib dashboard controls.
"""

from __future__ import annotations

import tkinter as tk
from tkinter import ttk
from typing import Callable

from dashboard_font import font_family

def configure_styles(root: tk.Misc) -> None:
    """Configure the shared dark instrument-panel visual system."""
    style = ttk.Style(root)
    if "clam" in style.theme_names():
        style.theme_use("clam")
    body = font_family()
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
    root.option_add("*TCombobox*Listbox.background", "#0E1626")
    root.option_add("*TCombobox*Listbox.foreground", "#F1F6FF")
    root.option_add("*TCombobox*Listbox.selectBackground", "#67E8F9")
    root.option_add("*TCombobox*Listbox.selectForeground", "#07161C")
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
                         font=(font_family(), -16, "bold"))

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
                         font=(font_family(), -15))

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
            highlightthickness=0, font=(font_family(), -15), justify=tk.LEFT)
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
