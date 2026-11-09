"""@file Driver-free Windows Local Simulator byte-stream client."""

from __future__ import annotations

import ctypes
import sys
import time


LOCAL_SIMULATOR_NAME = "Local Simulator"
LOCAL_SIMULATOR_PIPE = r"\\.\pipe\ble-uart-local-simulator"


class LocalSimulatorPort:
    """Small pyserial-compatible wrapper around the Windows named pipe."""

    def __init__(self, timeout: float = 0.1) -> None:
        if sys.platform != "win32":
            raise OSError("Local Simulator named pipes are available on Windows only")
        from ctypes import wintypes

        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.CreateFileW.argtypes = (
            wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.LPVOID,
            wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE,
        )
        kernel32.CreateFileW.restype = wintypes.HANDLE
        kernel32.PeekNamedPipe.argtypes = (
            wintypes.HANDLE, wintypes.LPVOID, wintypes.DWORD,
            ctypes.POINTER(wintypes.DWORD), ctypes.POINTER(wintypes.DWORD),
            ctypes.POINTER(wintypes.DWORD),
        )
        kernel32.PeekNamedPipe.restype = wintypes.BOOL
        kernel32.ReadFile.argtypes = (
            wintypes.HANDLE, wintypes.LPVOID, wintypes.DWORD,
            ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID,
        )
        kernel32.ReadFile.restype = wintypes.BOOL
        kernel32.WriteFile.argtypes = (
            wintypes.HANDLE, wintypes.LPCVOID, wintypes.DWORD,
            ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID,
        )
        kernel32.WriteFile.restype = wintypes.BOOL
        kernel32.CloseHandle.argtypes = (wintypes.HANDLE,)
        kernel32.CloseHandle.restype = wintypes.BOOL
        self._kernel32 = kernel32
        self._wintypes = wintypes
        self.timeout = timeout
        self.port = LOCAL_SIMULATOR_NAME
        self._handle = kernel32.CreateFileW(
            LOCAL_SIMULATOR_PIPE, 0xC0000000, 0, None, 3, 0, None)
        invalid = ctypes.c_void_p(-1).value
        if self._handle == invalid:
            error = ctypes.get_last_error()
            raise OSError(error, "Start the BLE UART server before connecting")

    @property
    def in_waiting(self) -> int:
        """Return the bytes immediately readable without blocking Tk."""
        available = self._wintypes.DWORD()
        if not self._kernel32.PeekNamedPipe(
                self._handle, None, 0, None, ctypes.byref(available), None):
            raise OSError(ctypes.get_last_error(), "Local Simulator disconnected")
        return int(available.value)

    def read(self, size: int = 1) -> bytes:
        """Read up to size bytes with the configured pyserial-like timeout."""
        deadline = time.monotonic() + self.timeout
        available = self.in_waiting
        while available == 0 and time.monotonic() < deadline:
            time.sleep(0.001)
            available = self.in_waiting
        if available == 0:
            return b""
        requested = min(size, available)
        buffer = ctypes.create_string_buffer(requested)
        received = self._wintypes.DWORD()
        if not self._kernel32.ReadFile(
                self._handle, buffer, requested, ctypes.byref(received), None):
            raise OSError(ctypes.get_last_error(), "Local Simulator read failed")
        return buffer.raw[:received.value]

    def write(self, data: bytes) -> int:
        """Write a complete protocol span to the local server engine."""
        buffer = ctypes.create_string_buffer(data)
        sent = self._wintypes.DWORD()
        if not self._kernel32.WriteFile(
                self._handle, buffer, len(data), ctypes.byref(sent), None):
            raise OSError(ctypes.get_last_error(), "Local Simulator write failed")
        if sent.value != len(data):
            raise OSError("Local Simulator accepted only a partial write")
        return int(sent.value)

    def flush(self) -> None:
        """Named-pipe writes are synchronous, so no extra flush is needed."""

    def close(self) -> None:
        """Close the Windows endpoint exactly once."""
        if self._handle is not None:
            self._kernel32.CloseHandle(self._handle)
            self._handle = None

    def __enter__(self) -> "LocalSimulatorPort":
        """Support the same context-manager contract as ``serial.Serial``."""
        return self

    def __exit__(self, _kind: object, _value: object, _traceback: object) -> None:
        self.close()
