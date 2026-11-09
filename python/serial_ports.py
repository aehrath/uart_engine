"""@file @brief Serial-port discovery used by the Python desktop client."""

from typing import Iterable


def available_ports() -> Iterable[tuple[str, str]]:
    """Enumerate serial devices visible through pyserial.

    @return Iterable of `(device path, human-readable description)` tuples; an
        empty iterable when pyserial is unavailable or no ports exist.
    """
    try:
        from serial.tools import list_ports
    except ImportError:
        return []
    ports = [(port.device, port.description) for port in list_ports.comports()]
    import sys
    if sys.platform == "win32":
        ports.insert(0, ("Local Simulator", "Built-in driver-free local server"))
    return ports
