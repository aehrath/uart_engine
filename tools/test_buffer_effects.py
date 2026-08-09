#!/usr/bin/env python3
"""Verify realistic ring-overwrite and DMA-interrupt behavior end to end."""

from __future__ import annotations

import argparse
import pathlib
import queue
import re
import subprocess
import sys
import threading
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "python"))

import serial  # noqa: E402
from ble_protocol import H4CrcStreamParser, build_test_transfer_frames  # noqa: E402


def _read_output(process: subprocess.Popen[str], lines: queue.Queue[str]) -> None:
    """Retain every line produced by the server without blocking its engine."""
    assert process.stdout is not None
    for line in process.stdout:
        lines.put(line.rstrip())


def _wait_line(lines: queue.Queue[str], marker: str, timeout: float = 5.0) -> str:
    """Wait for one server-output line containing ``marker``."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            line = lines.get(timeout=max(0.01, deadline - time.monotonic()))
        except queue.Empty as error:
            raise TimeoutError(f"server did not report {marker!r}") from error
        if marker in line:
            return line
    raise TimeoutError(f"server did not report {marker!r}")


def _open_port(device: str) -> serial.Serial:
    """Open a pseudo-UART while tolerating macOS custom-baud rejection."""
    try:
        return serial.Serial(device, 4_000_000, timeout=0.05, write_timeout=2.0)
    except (OSError, serial.SerialException):
        return serial.Serial(device, 115_200, timeout=0.05, write_timeout=2.0)


def _completion_received(port: serial.Serial, identifier: int,
                         timeout: float) -> bool:
    """Return whether a matching successful transfer completion arrived."""
    parser = H4CrcStreamParser()
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        for frame in parser.feed(port.read(4096)):
            if frame.test_completion == (identifier, 0, 128):
                return True
    return False


def run_case(server: pathlib.Path, ring: int, dma: int,
             expect_success: bool) -> None:
    """Run one isolated 128-byte transfer and verify its hardware effects."""
    process = subprocess.Popen(
        [str(server)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, text=True, bufsize=1)
    lines: queue.Queue[str] = queue.Queue()
    threading.Thread(target=_read_output, args=(process, lines), daemon=True).start()
    try:
        port_line = _wait_line(lines, "type this port manually: ")
        device = port_line.split("type this port manually: ", 1)[1]
        identifier = 0x5100 + ring + dma
        frame = next(iter(build_test_transfer_frames(128, identifier)))
        with _open_port(device) as port:
            assert process.stdin is not None
            process.stdin.write(f"ring {ring}\ndma-buffer {dma}\ndma\n")
            process.stdin.flush()
            time.sleep(0.05)
            port.write(frame)
            port.flush()
            irq_line = _wait_line(lines, "RX IRQ TRANSMISSION")
            match = re.search(r"transmission=(\d+)", irq_line)
            if match is None:
                raise RuntimeError(f"malformed IRQ report: {irq_line}")
            expected_irqs = (len(frame) + dma - 1) // dma
            if int(match.group(1)) != expected_irqs:
                raise RuntimeError(
                    f"DMA {dma}: expected {expected_irqs} IRQs for {len(frame)} "
                    f"wire bytes, got {match.group(1)}")
            completed = _completion_received(port, identifier, 1.0)
            if completed != expect_success:
                raise RuntimeError(
                    f"ring {ring}: completion={completed}, expected={expect_success}")
            process.stdin.write("stats\n")
            process.stdin.flush()
            stats = _wait_line(lines, "RX PIPELINE")
            drops = re.search(r"ring_drop=(\d+)", stats)
            expected_drops = 0 if expect_success else 1
            if drops is None or int(drops.group(1)) != expected_drops:
                raise RuntimeError(
                    f"ring {ring}: expected ring_drop={expected_drops}: {stats}")
            print(f"ring={ring} dma={dma} wire={len(frame)} "
                  f"irqs={expected_irqs} completion={completed} "
                  f"ring_drop={expected_drops}")
    finally:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=3.0)
            except subprocess.TimeoutExpired:
                process.kill()


def main() -> int:
    """Verify DMA granularity and an undersized IDLE-delimited ring."""
    parser = argparse.ArgumentParser()
    parser.add_argument("server", type=pathlib.Path)
    args = parser.parse_args()
    run_case(args.server, ring=256, dma=32, expect_success=True)
    run_case(args.server, ring=256, dma=128, expect_success=True)
    run_case(args.server, ring=128, dma=32, expect_success=False)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
