"""NXP LPUART divisor and 8-N-1 sampler mirroring lpuart_sim.c."""

from __future__ import annotations

from dataclasses import dataclass

MIN_OSR = 4
MAX_OSR = 32
MAX_SBR = 8191
MAX_SOURCE_CLOCK_HZ = 100_000_000
MAX_REQUESTED_BAUD = 25_000_000


@dataclass
class SimulationResult:
    """Bytes and UART framing errors produced by one sampled packet."""

    data: bytes
    changed_bytes: int
    framing_errors: int

    @property
    def valid(self) -> bool:
        """Return whether all data and stop-bit samples were valid."""
        return self.changed_bytes == 0 and self.framing_errors == 0


@dataclass
class LpuartSimulator:
    """Selected KW47 timing with persistent asynchronous sample phase."""

    source_clock_hz: int
    requested_baud: int
    osr: int
    sbr: int
    actual_baud: int
    error_ppm: int
    phase: int = 0

    @classmethod
    def configure(cls, source_clock_hz: int, requested_baud: int) -> "LpuartSimulator":
        """Select the closest OSR/SBR pair supported by the KW47 LPUART."""
        if not 0 < source_clock_hz <= MAX_SOURCE_CLOCK_HZ:
            raise ValueError("source clock is outside the simulator range")
        if not 0 < requested_baud <= MAX_REQUESTED_BAUD:
            raise ValueError("requested baud is outside the simulator range")
        best: tuple[int, int, int] | None = None
        for osr in range(MIN_OSR, MAX_OSR + 1):
            for sbr in range(1, MAX_SBR + 1):
                divisor = osr * sbr
                error = abs(source_clock_hz - requested_baud * divisor)
                if best is None or error * (best[1] * best[2]) < best[0] * divisor or (
                        error * (best[1] * best[2]) == best[0] * divisor
                        and osr > best[1]):
                    best = (error, osr, sbr)
        assert best is not None
        _error, osr, sbr = best
        divisor = osr * sbr
        difference = source_clock_hz - requested_baud * divisor
        return cls(source_clock_hz, requested_baud, osr, sbr,
                   (source_clock_hz + divisor // 2) // divisor,
                   int(difference * 1_000_000 / (requested_baud * divisor)))

    def receive(self, packet: bytes) -> SimulationResult:
        """Sample a continuous 8-N-1 packet without discarding invalid bytes."""
        denominator = self.requested_baud * self.sbr
        phase_step = (10 * self.source_clock_hz) % denominator

        def line_level(bit_index: int) -> int:
            byte_index, bit_in_character = divmod(bit_index, 10)
            if byte_index >= len(packet) or bit_in_character == 9:
                return 1
            if bit_in_character == 0:
                return 0
            return (packet[byte_index] >> (bit_in_character - 1)) & 1

        sampled_packet = bytearray(len(packet))
        changed = 0
        framing = 0
        for byte_index, transmitted in enumerate(packet):
            delay = 0 if self.phase == 0 else denominator - self.phase
            sampled = 0
            for data_bit in range(8):
                sample_offset = ((3 + 2 * data_bit) * self.osr + 1) // 2
                relative_bit = (delay + sample_offset * denominator) // self.source_clock_hz
                sampled |= line_level(byte_index * 10 + relative_bit) << data_bit
            stop_offset = (19 * self.osr + 1) // 2
            stop_bit = (delay + stop_offset * denominator) // self.source_clock_hz
            if line_level(byte_index * 10 + stop_bit) == 0:
                framing += 1
            sampled_packet[byte_index] = sampled
            changed += sampled != transmitted
            self.phase = (self.phase + phase_step) % denominator
        return SimulationResult(bytes(sampled_packet), changed, framing)
