"""Shared rounded dropdown mirroring dashboard_dropdown.c."""

from __future__ import annotations

import tkinter as tk

from dashboard_font import font_family

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
            highlightthickness=0, font=(font_family(), -15),
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
                                   font=(font_family(), -15))

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

