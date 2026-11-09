#!/usr/bin/env python3
"""Verify overload creates queue pressure without freezing server controls."""

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
from ble_protocol import build_test_transfer_frames  # noqa: E402
from local_simulator import LOCAL_SIMULATOR_NAME, LocalSimulatorPort  # noqa: E402


def read_engine(process: subprocess.Popen[str], lines: queue.Queue[str]) -> None:
    """Drain engine output continuously so logging cannot create backpressure."""
    assert process.stdout is not None
    for line in process.stdout:
        lines.put(line.rstrip())


def wait_for_endpoint(lines: queue.Queue[str], timeout: float = 5.0) -> str:
    """Return the local endpoint announced by a newly started engine."""
    deadline = time.monotonic() + timeout
    marker = "Local simulator endpoint: "
    while time.monotonic() < deadline:
        line = lines.get(timeout=max(0.01, deadline - time.monotonic()))
        if marker in line:
            return line.split(marker, 1)[1]
    raise TimeoutError("server did not publish a local endpoint")


def main() -> int:
    """Drive the formerly freezing 24 ms workload and prove control liveness."""
    parser = argparse.ArgumentParser()
    parser.add_argument("server")
    parser.add_argument("--load-ms", type=int, default=24)
    parser.add_argument("--require-pressure", action=argparse.BooleanOptionalAction,
                        default=True)
    args = parser.parse_args()
    process = subprocess.Popen(
        [args.server], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, text=True, bufsize=1)
    lines: queue.Queue[str] = queue.Queue()
    threading.Thread(target=read_engine, args=(process, lines), daemon=True).start()
    try:
        endpoint = wait_for_endpoint(lines)
        try:
            port_context = (LocalSimulatorPort(timeout=0.05)
                            if endpoint == LOCAL_SIMULATOR_NAME else serial.Serial(
                                endpoint, 4_000_000, timeout=0.05,
                                write_timeout=5.0))
        except (OSError, serial.SerialException):
            port_context = serial.Serial(
                endpoint, 115_200, timeout=0.05, write_timeout=5.0)

        with port_context as port:
            assert process.stdin is not None
            process.stdin.write(
                "baud 4000000\nring 1024\ndma-buffer 128\n"
                f"dma\nload {args.load_ms} {args.load_ms} 0\n")
            process.stdin.flush()
            time.sleep(0.05)

            started = time.monotonic()
            logical_bytes = 0
            batch = bytearray()
            for frame in build_test_transfer_frames(512 * 1024, identifier=0x5151):
                batch.extend(frame)
                logical_bytes += int.from_bytes(frame[21:23], "little")
                if len(batch) >= 64 * 1024:
                    delay = started + logical_bytes * 8 / 4_000_000 - time.monotonic()
                    if delay > 0:
                        time.sleep(delay)
                    port.write(batch)
                    batch.clear()
            if batch:
                delay = started + logical_bytes * 8 / 4_000_000 - time.monotonic()
                if delay > 0:
                    time.sleep(delay)
                port.write(batch)
            port.flush()

            process.stdin.write("stats\nload 0 0 0\nload status\n")
            process.stdin.flush()
            deadline = time.monotonic() + 5.0
            queue_peak = 0
            ring_peak = 0
            load_disabled = False
            observed: list[str] = []
            pattern = re.compile(
                r"queue=\d+/(\d+) queue_peak=(\d+).*"
                r"ring=\d+/(\d+) ring_peak=(\d+)")
            while time.monotonic() < deadline and not (queue_peak and load_disabled):
                try:
                    line = lines.get(timeout=max(0.01, deadline - time.monotonic()))
                except queue.Empty:
                    break
                observed.append(line)
                match = pattern.search(line)
                if match:
                    queue_peak = max(queue_peak, int(match.group(2)))
                    ring_peak = max(ring_peak, int(match.group(4)))
                if "THREAD LOAD ->" in line and "max_ms=0" in line:
                    load_disabled = True

            if args.require_pressure and queue_peak == 0:
                raise RuntimeError(
                    "busy workload did not create queue pressure; engine said: "
                    + " | ".join(observed[-12:]))
            if args.require_pressure and ring_peak == 0:
                raise RuntimeError("busy workload did not record ring pressure")
            if not load_disabled or process.poll() is not None:
                raise RuntimeError("server control task became unresponsive")
            print(f"busy-load liveness: queue_peak={queue_peak}/16, "
                  f"ring_peak={ring_peak}/1024, "
                  "load disabled successfully, server responsive")
        return 0
    finally:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=3.0)
            except subprocess.TimeoutExpired:
                process.kill()


if __name__ == "__main__":
    raise SystemExit(main())
