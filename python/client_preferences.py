"""Persistent BLE UART client preferences in the platform user-config folder."""

from __future__ import annotations

import os
from pathlib import Path
import sys
from dataclasses import dataclass


@dataclass
class DashboardSettings:
    """Persisted operating controls shared with the native dashboards."""

    clock: int = 20_000_000
    baud: int = 4_000_000
    ring: int = 16 * 1024
    dma: int = 256
    bulk: int = 2 * 1024 * 1024
    byte_irq: bool = False
    load_weight: int = 0
    load_min_ms: int = 0
    load_max_ms: int = 0


def couple_busy_range(minimum: int, maximum: int,
                      changed: str) -> tuple[int, int]:
    """Keep the edited min/max range ordered without swapping its endpoints."""
    if changed == "minimum" and minimum > maximum:
        maximum = minimum
    elif changed == "maximum" and maximum < minimum:
        minimum = maximum
    return minimum, maximum


def preference_path() -> Path:
    """Return the platform-native filename used for the selected UART port."""
    if sys.platform == "win32":
        base = Path(os.environ.get("APPDATA", Path.home())) / "BLE UART"
    elif sys.platform == "darwin":
        base = Path.home() / "Library" / "Application Support" / "BLE UART"
    else:
        configured = os.environ.get("XDG_CONFIG_HOME")
        base = (Path(configured) if configured else Path.home() / ".config") / "ble-uart"
    return base / "client-port.txt"


def load_port(default: str) -> str:
    """Load a nonempty saved port, falling back to ``default`` on any error."""
    try:
        saved = preference_path().read_text(encoding="utf-8").splitlines()[0].strip()
        return saved or default
    except (IndexError, OSError, UnicodeError):
        return default


def save_port(port: str) -> bool:
    """Persist one selected port without allowing embedded line separators."""
    selected = port.strip()
    if not selected or "\n" in port or "\r" in port:
        return False
    try:
        path = preference_path()
        path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        path.write_text(selected + "\n", encoding="utf-8")
        return True
    except (OSError, UnicodeError):
        return False


def load_settings(scope: str) -> DashboardSettings:
    """Load client or server settings, retaining defaults for invalid fields."""
    settings = DashboardSettings()
    try:
        values = dict(line.split("=", 1) for line in
                      (preference_path().parent / f"{scope}-settings.txt")
                      .read_text(encoding="utf-8").splitlines() if "=" in line)
        settings.clock = int(values.get("clock", settings.clock))
        settings.baud = int(values.get("baud", settings.baud))
        settings.ring = int(values.get("ring", settings.ring))
        settings.dma = int(values.get("dma", settings.dma))
        settings.bulk = int(values.get("bulk", settings.bulk))
        settings.byte_irq = int(values.get("byte_irq", "0")) != 0
        settings.load_weight = int(values.get("load_weight", settings.load_weight))
        settings.load_min_ms = int(values.get(
            "load_min_ms", settings.load_min_ms))
        settings.load_max_ms = int(values.get(
            "load_max_ms", settings.load_max_ms))
        if not (values.get("load_model") == "3"
                and 0 <= settings.load_weight <= 100
                and (settings.load_max_ms == 0 or
                     0 <= settings.load_min_ms <= settings.load_max_ms)
                and settings.load_max_ms <= 0xFFFFFFFF):
            defaults = DashboardSettings()
            settings.load_weight = defaults.load_weight
            settings.load_min_ms = defaults.load_min_ms
            settings.load_max_ms = defaults.load_max_ms
    except (OSError, UnicodeError, ValueError):
        return DashboardSettings()
    return settings


def save_settings(scope: str, settings: DashboardSettings) -> bool:
    """Persist one dashboard's independent operating controls."""
    try:
        path = preference_path().parent / f"{scope}-settings.txt"
        path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        path.write_text(
            f"clock={settings.clock}\nbaud={settings.baud}\n"
            f"ring={settings.ring}\ndma={settings.dma}\n"
            f"bulk={settings.bulk}\nbyte_irq={int(settings.byte_irq)}\n"
            f"load_model=3\nload_weight={settings.load_weight}\n"
            f"load_min_ms={settings.load_min_ms}\n"
            f"load_max_ms={settings.load_max_ms}\n",
            encoding="utf-8")
        return True
    except (OSError, UnicodeError):
        return False
