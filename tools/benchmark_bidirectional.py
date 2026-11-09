#!/usr/bin/env python3
"""Benchmark identical deterministic traffic in both local-link directions."""

from __future__ import annotations

import argparse
import pathlib
import queue
import subprocess
import sys
import threading
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "python"))

import serial  # noqa: E402
from ble_protocol import H4CrcStreamParser, build_test_transfer_frames  # noqa: E402
from local_simulator import LOCAL_SIMULATOR_NAME, LocalSimulatorPort  # noqa: E402


def read_engine(process: subprocess.Popen[str], lines: queue.Queue[str]) -> None:
    """Forward server output without allowing its pipe to back up."""
    assert process.stdout is not None
    for line in process.stdout:
        lines.put(line.rstrip())


def wait_for_port(lines: queue.Queue[str], timeout: float = 5.0) -> str:
    """Extract the local endpoint announced by the server."""
    deadline = time.monotonic() + timeout
    marker = "Local simulator endpoint: "
    while time.monotonic() < deadline:
        line = lines.get(timeout=max(0.01, deadline - time.monotonic()))
        if marker in line:
            return line.split(marker, 1)[1]
    raise TimeoutError("server did not publish a local endpoint")


def receive_until(port: object, predicate: object, timeout: float) -> tuple[int, int]:
    """Parse CRC-framed traffic until predicate accepts a frame."""
    parser = H4CrcStreamParser()
    packets = 0
    wire_bytes = 0
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        data = port.read(64 * 1024)  # type: ignore[attr-defined]
        wire_bytes += len(data)
        for frame in parser.feed(data):
            packets += 1
            if not frame.crc_valid:
                raise RuntimeError("received a CRC-invalid benchmark frame")
            if callable(predicate) and predicate(frame):
                return packets, wire_bytes
    raise TimeoutError("timed out waiting for transfer completion")


def main() -> int:
    """Run client→server and server→client transfers of the same size."""
    parser = argparse.ArgumentParser()
    parser.add_argument("server", type=pathlib.Path)
    parser.add_argument("--size", type=int, default=2 * 1024 * 1024)
    parser.add_argument("--baud", type=int, default=4_000_000)
    parser.add_argument("--mode", choices=("dma", "irq"), default="dma")
    parser.add_argument("--ring", type=int, default=16 * 1024)
    parser.add_argument("--dma", type=int, default=256)
    parser.add_argument("--load-min-ms", type=int, default=0)
    parser.add_argument("--load-max-ms", type=int, default=0)
    parser.add_argument("--load-weight", type=int, default=0)
    parser.add_argument("--max-ratio", type=float, default=1.15)
    args = parser.parse_args()
    process = subprocess.Popen(
        [str(args.server)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, text=True, bufsize=1)
    lines: queue.Queue[str] = queue.Queue()
    threading.Thread(target=read_engine, args=(process, lines), daemon=True).start()
    try:
        device = wait_for_port(lines)
        try:
            port_context = (LocalSimulatorPort(timeout=0.05)
                            if device == LOCAL_SIMULATOR_NAME else serial.Serial(
                                device, 4_000_000, timeout=0.05,
                                write_timeout=5.0))
        except (OSError, serial.SerialException):
            # macOS pseudo-terminals reject IOSSIOSPEED even though their byte
            # transport is not physically baud-limited.
            port_context = serial.Serial(
                device, 115_200, timeout=0.05, write_timeout=5.0)
        with port_context as port:
            assert process.stdin is not None
            process.stdin.write(
                f"baud {args.baud}\nring {args.ring}\n"
                f"dma-buffer {args.dma}\n{args.mode}\n"
                f"load {args.load_min_ms} {args.load_max_ms} "
                f"{args.load_weight}\n")
            process.stdin.flush()
            time.sleep(0.05)
            frames = build_test_transfer_frames(args.size, identifier=0x4242)
            started = time.monotonic()
            batch = bytearray()
            sent_packets = 0
            sent_bytes = 0
            sent_wire_bytes = 0
            for frame in frames:
                batch.extend(frame)
                sent_packets += 1
                sent_wire_bytes += len(frame)
                sent_bytes += int.from_bytes(frame[21:23], "little")
                if len(batch) >= 64 * 1024:
                    delay = started + sent_bytes * 8 / args.baud - time.monotonic()
                    if delay > 0:
                        time.sleep(delay)
                    port.write(batch)
                    batch.clear()
            if batch:
                delay = started + sent_bytes * 8 / args.baud - time.monotonic()
                if delay > 0:
                    time.sleep(delay)
                port.write(batch)
            port.flush()
            completion = lambda frame: frame.test_completion == (0x4242, 0, args.size)
            _, _ = receive_until(port, completion, 20.0)
            client_seconds = time.monotonic() - started

            started = time.monotonic()
            process.stdin.write(f"test {args.size}\n")
            process.stdin.flush()
            end_frame = lambda frame: (
                frame.test_transfer_info is not None
                and frame.test_transfer_info[0] == args.size
                and frame.test_transfer_info[3] & 0x02)
            received_packets, wire_bytes = receive_until(port, end_frame, 20.0)
            server_seconds = time.monotonic() - started

        print(f"client->server: {args.size} bytes, {sent_packets} packets, "
              f"{client_seconds:.3f}s, {args.size/client_seconds/1_000_000:.3f} MB/s")
        print(f"server->client: {args.size} bytes, {received_packets} packets, "
              f"{server_seconds:.3f}s, {args.size/server_seconds/1_000_000:.3f} MB/s, "
              f"wire={wire_bytes}")
        ratio = max(client_seconds, server_seconds) / min(client_seconds, server_seconds)
        print(f"direction ratio: {ratio:.3f}x")
        if received_packets != sent_packets or wire_bytes != sent_wire_bytes:
            raise RuntimeError(
                "directions did not use identical packetization and wire length")
        if ratio > args.max_ratio:
            raise RuntimeError(
                f"direction timing ratio {ratio:.3f} exceeds {args.max_ratio:.3f}")
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
