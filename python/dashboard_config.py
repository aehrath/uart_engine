"""Shared dashboard presets and size choices mirroring dashboard_config.c."""

from __future__ import annotations

COMMAND_PRESETS: dict[str, tuple[int, bytes]] = {
    "HCI Reset": (0x0C03, b""),
    "Read Local Version": (0x1001, b""),
    "Read BD_ADDR": (0x1009, b""),
    "LE Read Buffer Size": (0x2002, b""),
    "LE Enable Scan": (0x200C, bytes((0x01, 0x00))),
    "LE Disable Scan": (0x200C, bytes((0x00, 0x00))),
}

TEST_TRANSFER_SIZES: tuple[tuple[str, int], ...] = (
    ("16 KiB", 16 * 1024), ("64 KiB", 64 * 1024),
    ("128 KiB", 128 * 1024), ("512 KiB", 512 * 1024),
    ("1 MiB", 1 * 1024 * 1024), ("2 MiB", 2 * 1024 * 1024),
    ("4 MiB", 4 * 1024 * 1024), ("8 MiB", 8 * 1024 * 1024),
    ("16 MiB", 16 * 1024 * 1024),
)

RING_BUFFER_SIZES: tuple[tuple[str, int], ...] = (
    ("256 B", 256), ("1 KiB", 1024), ("4 KiB", 4096),
    ("16 KiB", 16 * 1024), ("64 KiB", 64 * 1024),
)

DMA_BUFFER_SIZES: tuple[tuple[str, int], ...] = (
    ("32 B", 32), ("64 B", 64), ("128 B", 128), ("256 B", 256),
    ("512 B", 512), ("1 KiB", 1024), ("4 KiB", 4096),
)
