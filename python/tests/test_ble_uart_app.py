"""@file @brief Unit tests for Python CRC, framing, parsing, and bulk transfers."""

import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from ble_protocol import (  # noqa: E402
    COMMAND_PRESETS,
    H4CrcStreamParser,
    TEST_TRANSFER_SIZES,
    RX_MODE_BYTE_IRQ,
    RX_MODE_QUERY,
    add_crc,
    build_hci_command,
    build_rx_mode_command,
    build_test_transfer_frames,
    crc16_ccitt,
    parse_hex,
)
from dashboard_config import DMA_BUFFER_SIZES, RING_BUFFER_SIZES  # noqa: E402
from lpuart_sim import LpuartSimulator  # noqa: E402


class ProtocolTests(unittest.TestCase):
    """Regression tests for the Python implementation of the shared wire protocol."""

    def test_crc_known_vector(self):
        """Verify the standard CRC16-CCITT `123456789` check value."""
        self.assertEqual(crc16_ccitt(b"123456789"), 0x29B1)

    def test_build_reset_command(self):
        """Verify HCI Reset framing and its appended CRC."""
        packet = build_hci_command(0x0C03)
        self.assertEqual(packet[:4], bytes.fromhex("01 03 0C 00"))
        self.assertEqual(packet[-2:], crc16_ccitt(packet[:-2]).to_bytes(2, "big"))

    def test_command_presets_match_native_client(self):
        """Keep the Python preset picker aligned with the portable C UI."""
        self.assertEqual(tuple(COMMAND_PRESETS), (
            "HCI Reset",
            "Read Local Version",
            "Read BD_ADDR",
            "LE Read Buffer Size",
            "LE Enable Scan",
            "LE Disable Scan",
        ))

    def test_transfer_sizes_match_native_client(self):
        """Keep every large-block dropdown label and size aligned with the C UI."""
        self.assertEqual(TEST_TRANSFER_SIZES, (
            ("16 KiB", 16 * 1024),
            ("64 KiB", 64 * 1024),
            ("128 KiB", 128 * 1024),
            ("512 KiB", 512 * 1024),
            ("1 MiB", 1 * 1024 * 1024),
            ("2 MiB", 2 * 1024 * 1024),
            ("4 MiB", 4 * 1024 * 1024),
            ("8 MiB", 8 * 1024 * 1024),
            ("16 MiB", 16 * 1024 * 1024),
        ))

    def test_receive_buffer_sizes_match_native_apps(self):
        """Keep the Python ring and DMA selectors aligned with both C UIs."""
        self.assertEqual(dict(RING_BUFFER_SIZES)["16 KiB"], 16 * 1024)
        self.assertEqual(dict(RING_BUFFER_SIZES)["64 KiB"], 64 * 1024)
        self.assertEqual(dict(DMA_BUFFER_SIZES)["256 B"], 256)
        self.assertEqual(dict(DMA_BUFFER_SIZES)["4 KiB"], 4 * 1024)

    def test_kw47_lpuart_clock_sampling(self):
        """Match native OSR/SBR selection and sampled packet failures."""
        packet = bytes.fromhex("01 03 0C 00 3E A5 5A FF")
        exact = LpuartSimulator.configure(20_000_000, 4_000_000)
        self.assertEqual((exact.osr, exact.sbr, exact.actual_baud),
                         (5, 1, 4_000_000))
        self.assertTrue(exact.receive(packet).valid)
        maximum = LpuartSimulator.configure(20_000_000, 5_000_000)
        self.assertEqual((maximum.osr, maximum.sbr, maximum.actual_baud),
                         (4, 1, 5_000_000))
        below_ceiling = LpuartSimulator.configure(15_900_000, 4_000_000)
        self.assertTrue(below_ceiling.receive(packet).valid)
        starved = LpuartSimulator.configure(12_000_000, 4_000_000)
        result = starved.receive(packet)
        self.assertEqual((starved.osr, starved.sbr, starved.actual_baud),
                         (4, 1, 3_000_000))
        self.assertFalse(result.valid)

    def test_rx_mode_command_and_event(self):
        """Verify typed mode requests and synchronized RXMD event decoding."""
        command = build_rx_mode_command(RX_MODE_QUERY)
        self.assertEqual(command[:5], bytes.fromhex("01 01 FC 01 FF"))
        parser = H4CrcStreamParser()
        event = add_crc(bytes.fromhex("04 FF 05 52 58 4D 44 01"))
        frame = parser.feed(event)[0]
        self.assertEqual(frame.rx_mode, RX_MODE_BYTE_IRQ)
        self.assertEqual(frame.summary, "RX mode: per-byte IRQ")

    def test_test_completion_event(self):
        """Decode the server acknowledgement used during connection verification."""
        parser = H4CrcStreamParser()
        event = add_crc(
            bytes.fromhex("04 FF 0B 54 45 53 54 34 12 00 80 00 00 00"))
        frame = parser.feed(event)[0]
        self.assertTrue(frame.crc_valid)
        self.assertEqual(frame.test_completion, (0x1234, 0, 128))
        self.assertEqual(frame.summary, "Test transfer 4660 passed, 128 bytes")

    def test_hex_input_formats(self):
        """Verify all separators accepted by the user-facing hex parser."""
        self.assertEqual(parse_hex("0x01, 03:0C 00"), bytes.fromhex("01 03 0C 00"))

    def test_fragmented_event(self):
        """Verify that one Event can span multiple serial reads."""
        parser = H4CrcStreamParser()
        event = add_crc(bytes.fromhex("04 0E 04 01 03 0C 00"))
        self.assertEqual(parser.feed(event[:2]), [])
        self.assertEqual(parser.feed(event[2:5]), [])
        frames = parser.feed(event[5:])
        self.assertEqual(len(frames), 1)
        self.assertTrue(frames[0].crc_valid)
        self.assertEqual(frames[0].summary, "Event 0x0E, 4 parameter byte(s)")

    def test_back_to_back_frames_and_bad_crc(self):
        """Verify resynchronization, coalesced frames, and invalid CRC reporting."""
        parser = H4CrcStreamParser()
        first = add_crc(bytes.fromhex("04 05 00"))
        second = bytearray(add_crc(bytes.fromhex("04 3E 01 02")))
        second[-1] ^= 0xFF
        frames = parser.feed(b"\xFF" + first + second)
        self.assertEqual(len(frames), 2)
        self.assertTrue(frames[0].crc_valid)
        self.assertFalse(frames[1].crc_valid)
        self.assertEqual(parser.discarded_bytes, 1)

    def test_bulk_transfer_framing(self):
        """Verify chunk count, flags, size bounds, and CRC for a bulk transfer."""
        frames = list(build_test_transfer_frames(512, identifier=7))
        self.assertEqual(len(frames), 3)
        self.assertTrue(all(len(frame) <= 255 for frame in frames))
        self.assertEqual(frames[0][0], 0x02)
        self.assertEqual(frames[0][5:9], b"TEST")
        self.assertEqual(frames[0][10] & 0x01, 0x01)
        self.assertEqual(frames[-1][10] & 0x02, 0x02)
        parser = H4CrcStreamParser()
        parsed = [parser.feed(frame)[0] for frame in frames]
        self.assertEqual(parsed[0].test_transfer_flags, 0x01)
        self.assertEqual(parsed[-1].test_transfer_flags, 0x02)
        self.assertEqual(parsed[0].test_transfer_info, (512, 0, 230, 0x01))
        self.assertEqual(parsed[-1].test_transfer_info, (512, 460, 52, 0x02))
        self.assertTrue(all(crc16_ccitt(frame[:-2]) == int.from_bytes(frame[-2:], "big")
                            for frame in frames))

    def test_coalesced_large_receive_batch(self):
        """Parse a large serial read without losing any back-to-back frames."""
        total_size = 512 * 1024
        wire = b"".join(build_test_transfer_frames(total_size, identifier=19))
        frames = H4CrcStreamParser().feed(wire)
        remainder = total_size % 230
        self.assertEqual(len(frames), (total_size + 229) // 230)
        self.assertTrue(all(frame.crc_valid for frame in frames))
        self.assertEqual(frames[-1].test_transfer_info,
                         (total_size, total_size - remainder, remainder, 0x02))


if __name__ == "__main__":
    unittest.main()
