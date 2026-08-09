"""Thread-safe producer/consumer telemetry and matching Tk visualization."""

from __future__ import annotations

from collections import deque
import threading
import time
import tkinter as tk

from dashboard_font import font_family


class PipelineTelemetry:
    """Measure half-second producer/consumer rates and retained headroom."""

    def __init__(self) -> None:
        self.lock = threading.Lock()
        self.produced = 0
        self.consumed = 0
        self.produced_frames = 0
        self.producer_bps = 0
        self.consumer_bps = 0
        self.queue_depth = 0
        self.queue_capacity = 16
        self.ring_burst = 0
        self.ring_capacity = 16 * 1024
        self.queue_peak = 0
        self.ring_peak = 0
        self.dma_burst = 0
        self.dma_capacity = 256
        self.dma_peak = 0
        self.latency_us = 0
        self.latency_max_us = 0
        self.headroom_us: int | None = None
        self.crc_drops = 0
        self.queue_drops = 0
        self.ring_drops = 0
        self.producer_history: deque[float] = deque(maxlen=120)
        self.consumer_history: deque[float] = deque(maxlen=120)
        self.queue_history: deque[float] = deque(maxlen=120)
        self.ring_history: deque[float] = deque(maxlen=120)
        self.dma_history: deque[float] = deque(maxlen=120)
        self.sample_time = time.monotonic()

    def reset(self) -> None:
        """Clear counters and history while retaining configured capacities."""
        with self.lock:
            self.produced = self.consumed = self.produced_frames = 0
            self.producer_bps = self.consumer_bps = 0
            self.queue_depth = self.ring_burst = 0
            self.dma_burst = 0
            self.queue_peak = self.ring_peak = self.dma_peak = 0
            self.latency_us = self.latency_max_us = 0
            self.headroom_us = None
            self.crc_drops = self.queue_drops = self.ring_drops = 0
            self.producer_history.clear()
            self.consumer_history.clear()
            self.queue_history.clear()
            self.ring_history.clear()
            self.dma_history.clear()
            self.sample_time = time.monotonic()

    def record_arrival(self, wire_size: int) -> None:
        """Record one IDLE-delimited wire frame entering ring and DMA storage."""
        with self.lock:
            self.ring_burst = min(wire_size, self.ring_capacity)
            self.ring_peak = max(self.ring_peak, self.ring_burst)
            self.dma_burst = min(wire_size, self.dma_capacity)
            self.dma_peak = max(self.dma_peak, self.dma_burst)

    def record_produced(self, size: int, queued_packets: int = 1) -> None:
        """Record one validated H4 packet entering the 16-slot packet queue."""
        with self.lock:
            self.produced += size
            self.produced_frames += 1
            self.queue_depth = queued_packets
            self.queue_peak = max(self.queue_peak, queued_packets)

    def record_consumed(self, size: int, queued_packets: int,
                        queued_at_ns: int) -> None:
        """Record one framed consumer dequeue and its queue latency."""
        latency = max(0, (time.monotonic_ns() - queued_at_ns) // 1000)
        with self.lock:
            self.consumed += size
            self.queue_depth = queued_packets
            self.latency_us = latency
            self.latency_max_us = max(self.latency_max_us, latency)

    def sample(self) -> None:
        """Close the current interval and calculate measured time-to-overrun."""
        with self.lock:
            now = time.monotonic()
            elapsed = now - self.sample_time
            if elapsed < 0.5:
                return
            self.producer_bps = int(self.produced / elapsed)
            self.consumer_bps = int(self.consumed / elapsed)
            growth = max(0, self.producer_bps - self.consumer_bps)
            average_frame = (self.produced // self.produced_frames
                             if self.produced_frames else max(1, self.ring_peak))
            free = (max(0, self.queue_capacity - self.queue_depth) * average_frame
                    + max(0, self.ring_capacity - self.ring_burst))
            self.headroom_us = int(free * 1_000_000 / growth) if growth else None
            self.producer_history.append(float(self.producer_bps))
            self.consumer_history.append(float(self.consumer_bps))
            self.queue_history.append(
                min(100.0, 100.0 * self.queue_peak / self.queue_capacity)
                if self.queue_capacity else 0.0)
            self.ring_history.append(
                min(100.0, 100.0 * self.ring_peak / self.ring_capacity)
                if self.ring_capacity else 0.0)
            self.dma_history.append(
                min(100.0, 100.0 * self.dma_peak / self.dma_capacity)
                if self.dma_capacity else 0.0)
            # Retain short bursts for this paint interval; a quiet following
            # interval replaces both values with zero instead of leaving them stale.
            self.queue_depth = self.queue_peak
            self.ring_burst = self.ring_peak
            self.dma_burst = self.dma_peak
            self.produced = self.consumed = self.produced_frames = 0
            self.queue_peak = self.ring_peak = self.dma_peak = 0
            self.sample_time = now


class PipelineMonitor(tk.Canvas):
    """Render the same rate graph, gauges, and footer as the native apps."""

    def __init__(self, parent: tk.Misc, telemetry: PipelineTelemetry) -> None:
        super().__init__(parent, background="#141C2E", borderwidth=0,
                         highlightthickness=0)
        self.telemetry = telemetry
        self.bind("<Configure>", lambda _event: self.draw())

    @staticmethod
    def _rate(value: int) -> str:
        if value >= 1_000_000:
            return f"{value / 1_000_000:.2f} MB/s"
        if value >= 1_000:
            return f"{value / 1_000:.1f} kB/s"
        return f"{value} B/s"

    @staticmethod
    def _duration(value: int | None) -> str:
        if value is None:
            return "stable"
        if value >= 1_000_000:
            return f"{value / 1_000_000:.2f} s"
        if value >= 1_000:
            return f"{value / 1_000:.1f} ms"
        return f"{value} us"

    def _rounded(self, x0: float, y0: float, x1: float, y1: float,
                 radius: float, color: str) -> None:
        """Draw a filled rounded rectangle without platform widget borders."""
        radius = min(radius, (x1-x0)/2, (y1-y0)/2)
        self.create_rectangle(x0+radius, y0, x1-radius, y1,
                              fill=color, outline=color)
        self.create_rectangle(x0, y0+radius, x1, y1-radius,
                              fill=color, outline=color)
        for oval in ((x0, y0, x0+2*radius, y0+2*radius),
                     (x1-2*radius, y0, x1, y0+2*radius),
                     (x0, y1-2*radius, x0+2*radius, y1),
                     (x1-2*radius, y1-2*radius, x1, y1)):
            self.create_oval(*oval, fill=color, outline=color)

    def draw(self) -> None:
        """Redraw from a coherent telemetry snapshot."""
        self.telemetry.sample()
        with self.telemetry.lock:
            p = self.telemetry
            producer, consumer = list(p.producer_history), list(p.consumer_history)
            queue_usage, ring_usage, dma_usage = (
                list(p.queue_history), list(p.ring_history), list(p.dma_history))
            values = (p.producer_bps, p.consumer_bps, p.queue_depth,
                      p.queue_capacity, p.ring_burst, p.ring_capacity,
                      p.latency_us, p.headroom_us, p.crc_drops,
                      p.queue_drops, p.ring_drops)
        pbps, cbps, queued, qcap, burst, rcap, latency, headroom, crc, qdrop, rdrop = values
        self.delete("all")
        width, height = max(1, self.winfo_width()), max(1, self.winfo_height())
        body = font_family()
        self._rounded(0, 0, width-1, height-1, 8, "#070B14")
        self.create_text(12, 9, anchor=tk.NW, fill="#E8EFF9",
                         font=(body, -10, "bold"),
                         text=f"PRODUCER  {self._rate(pbps)}     CONSUMER  {self._rate(cbps)}")
        x0, y0, x1, y1 = 12, 28, width - 12, 71
        self._rounded(x0, y0, x1, y1, 5, "#0C1321")
        for x, label, color in ((x1-66, "Q%", "#A78BFA"),
                                (x1-44, "R%", "#FFC45C"),
                                (x1-22, "D%", "#9198A6")):
            self.create_text(x, y0+2, anchor=tk.NW, fill=color,
                             font=(body, -9, "bold"), text=label)
        self.create_line(x0, (y0 + y1) / 2, x1, (y0 + y1) / 2, fill="#31415C")
        maximum = max([1.0, *producer, *consumer])
        for samples, color in ((producer, "#4C94FF"), (consumer, "#2DD4BF")):
            if len(samples) > 1:
                points: list[float] = []
                for index, value in enumerate(samples):
                    points.extend((x0 + (x1-x0) * index / 119,
                                   y1 - (y1-y0) * value / maximum))
                self.create_line(*points, fill=color, width=2, smooth=True)
            if samples:
                x = x0 + (x1-x0) * (len(samples)-1) / 119
                y = y1 - (y1-y0) * samples[-1] / maximum
                self.create_oval(x-2.5, y-2.5, x+2.5, y+2.5,
                                 fill=color, outline=color)
        for samples, color, line_width in ((dma_usage, "#9198A6", 4),
                                           (ring_usage, "#FFC45C", 2),
                                           (queue_usage, "#A78BFA", 2)):
            if len(samples) > 1:
                points = []
                for index, value in enumerate(samples):
                    points.extend((x0 + (x1-x0) * index / 119,
                                   y1 - (y1-y0) * value / 100.0))
                self.create_line(*points, fill=color, width=line_width,
                                 smooth=True)
            if samples:
                x = x0 + (x1-x0) * (len(samples)-1) / 119
                y = y1 - (y1-y0) * samples[-1] / 100.0
                self.create_oval(x-2.5, y-2.5, x+2.5, y+2.5,
                                 fill=color, outline=color)

        def gauge(label: str, current: int, capacity: int, y: int, color: str) -> None:
            percentage = min(100.0, current * 100.0 / capacity) if capacity else 0.0
            self.create_text(12, y-2, anchor=tk.NW, fill="#8F9EB5",
                             font=(body, -10), text=f"{label} {percentage:.1f}%")
            gx0, gx1 = 106, width - 12
            self._rounded(gx0, y, gx1, y+8, 4, "#232D41")
            fraction = min(1.0, current / capacity) if capacity else 0.0
            if fraction:
                self._rounded(gx0, y, gx0+(gx1-gx0)*fraction, y+8, 4, color)

        gauge("Q PEAK", queued, qcap, 78, "#A78BFA")
        gauge("R PEAK", burst, rcap, 95, "#4C94FF")
        color = "#FF7878" if headroom is not None and headroom < 2_000_000 else (
            "#FFC45C" if headroom is not None and headroom < 10_000_000 else "#8F9EB5")
        self.create_text(12, 114, anchor=tk.NW, fill=color, font=(body, -10),
                         text=(f"LATENCY {self._duration(latency)}   "
                               f"HEADROOM {self._duration(headroom)}   "
                               f"DROPS {crc}/{qdrop}/{rdrop}"))
