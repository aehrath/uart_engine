"""@file @brief Unit tests for Python CRC, framing, parsing, and bulk transfers."""

import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from ble_protocol import (  # noqa: E402
    H4CrcStreamParser,
    add_crc,
    build_hci_command,
    build_test_transfer_frames,
    crc16_ccitt,
    parse_hex,
)


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
        self.assertTrue(all(crc16_ccitt(frame[:-2]) == int.from_bytes(frame[-2:], "big")
                            for frame in frames))


if __name__ == "__main__":
    unittest.main()
