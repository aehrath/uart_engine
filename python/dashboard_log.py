"""Bounded scroll-preserving dashboard log mirroring dashboard_log.c."""

from __future__ import annotations

from typing import Any


def append_log_text(widget: Any, line_count: int, message: str, kind: str,
                    limit: int = 256) -> int:
    """Append one tagged line and return the updated retained line count."""
    following_tail = widget.yview()[1] >= 0.999
    widget.configure(state="normal")
    widget.insert("end", f"{message}\n", kind)
    line_count += 1
    if line_count > limit:
        widget.delete("1.0", "2.0")
        line_count -= 1
    if following_tail:
        widget.see("end")
    widget.configure(state="disabled")
    return line_count
