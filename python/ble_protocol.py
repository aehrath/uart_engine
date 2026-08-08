"""@file
@brief BLE HCI-over-UART framing, CRC, parsing, and test-transfer helpers.

The module mirrors the packed C wire protocol for the optional Python client.
"""

from __future__ import annotations

import binascii
from dataclasses import dataclass
from typing import Iterable


H4_COMMAND = 0x01
H4_ACL = 0x02
H4_SCO = 0x03
H4_EVENT = 0x04
H4_ISO = 0x05
RX_MODE_OPCODE = 0xFC01
RX_MODE_DMA = 0x00
RX_MODE_BYTE_IRQ = 0x01
RX_MODE_QUERY = 0xFF
MAX_HCI_PAYLOAD = 4096
TEST_TRANSFER_HANDLE = 0x0FFE
TEST_TRANSFER_CHUNK_SIZE = 230

#: Ordered large-transfer choices shared with the portable C UI.
TEST_TRANSFER_SIZES: tuple[tuple[str, int], ...] = (
    ("16 KiB", 16 * 1024),
    ("64 KiB", 64 * 1024),
    ("128 KiB", 128 * 1024),
    ("512 KiB", 512 * 1024),
    ("1 MiB", 1 * 1024 * 1024),
    ("2 MiB", 2 * 1024 * 1024),
    ("4 MiB", 4 * 1024 * 1024),
    ("8 MiB", 8 * 1024 * 1024),
    ("16 MiB", 16 * 1024 * 1024),
)


def crc16_ccitt(data: bytes) -> int:
    """Compute CRC16-CCITT.

    @param data Bytes protected by the checksum.
    @return CRC using polynomial `0x1021` and initial value `0xffff`.
    """
    return binascii.crc_hqx(data, 0xFFFF)


def add_crc(payload: bytes) -> bytes:
    """Append the transport's big-endian CRC trailer.

    @param payload Complete H4 packet without a trailer.
    @return New immutable frame containing the packet and two-byte CRC.
    """
    crc = crc16_ccitt(payload)
    return payload + crc.to_bytes(2, "big")


def parse_hex(text: str) -> bytes:
    """Parse user-entered hexadecimal bytes.

    @param text Whitespace, comma, or colon-separated hexadecimal text.
    @return Parsed bytes.
    @throws ValueError If a token is incomplete or non-hexadecimal.
    """
    compact = text.replace("0x", "").replace(",", " ").replace(":", " ")
    try:
        return bytes.fromhex(compact)
    except ValueError as error:
        raise ValueError("Enter complete hexadecimal bytes, such as '01 03 0C 00'.") from error


def build_hci_command(opcode: int, parameters: bytes = b"") -> bytes:
    """Build one CRC-protected H4 Command frame.

    @param opcode Bluetooth command opcode in host byte order.
    @param parameters Zero through 255 command parameter bytes.
    @return Serialized H4 packet followed by its CRC.
    @throws ValueError If the opcode or parameter count is out of range.
    """
    if not 0 <= opcode <= 0xFFFF:
        raise ValueError("Opcode must be between 0x0000 and 0xffff.")
    if len(parameters) > 0xFF:
        raise ValueError("An HCI command can contain at most 255 parameter bytes.")
    h4_packet = bytes((H4_COMMAND, opcode & 0xFF, opcode >> 8, len(parameters))) + parameters
    return add_crc(h4_packet)


def build_rx_mode_command(mode: int) -> bytes:
    """Build the typed vendor command that sets or queries UART RX mode."""
    if mode not in (RX_MODE_DMA, RX_MODE_BYTE_IRQ, RX_MODE_QUERY):
        raise ValueError("Unsupported UART receive mode.")
    return build_hci_command(RX_MODE_OPCODE, bytes((mode,)))


def test_pattern_byte(offset: int) -> int:
    """Return the deterministic test byte for a logical transfer offset."""
    return (offset * 31 + 7) & 0xFF


def build_test_transfer_frames(total_size: int, identifier: int = 1) -> Iterable[bytes]:
    """Generate CRC-protected ACL frames for one deterministic test transfer.

    @param total_size Logical data size from one byte through four GiB.
    @param identifier Transfer identifier encoded in every chunk.
    @return Iterator of complete CRC-protected H4 ACL frames.
    @throws ValueError If @p total_size is outside the wire format's range.
    """
    if not 0 < total_size <= 0xFFFFFFFF:
        raise ValueError("Test transfer size must be between 1 and 4 GiB.")
    offset = 0
    while offset < total_size:
        chunk_size = min(TEST_TRANSFER_CHUNK_SIZE, total_size - offset)
        flags = (0x01 if offset == 0 else 0) | (0x02 if offset + chunk_size == total_size else 0)
        header = (
            b"TEST"
            + bytes((1, flags))
            + identifier.to_bytes(2, "little")
            + total_size.to_bytes(4, "little")
            + offset.to_bytes(4, "little")
            + chunk_size.to_bytes(2, "little")
        )
        payload = bytes(test_pattern_byte(offset + index) for index in range(chunk_size))
        acl_data = header + payload
        h4_packet = (
            bytes((H4_ACL,))
            + TEST_TRANSFER_HANDLE.to_bytes(2, "little")
            + len(acl_data).to_bytes(2, "little")
            + acl_data
        )
        yield add_crc(h4_packet)
        offset += chunk_size


@dataclass(frozen=True)
class ParsedFrame:
    """A complete H4 packet and the result of its CRC validation.

    @var packet_type H4 packet indicator.
    @var h4_packet Packet bytes excluding the CRC trailer.
    @var received_crc Big-endian CRC decoded from the wire.
    @var computed_crc CRC calculated from `h4_packet`.
    """
    packet_type: int
    h4_packet: bytes
    received_crc: int
    computed_crc: int

    @property
    def crc_valid(self) -> bool:
        """Return whether the received and computed CRC values match."""
        return self.received_crc == self.computed_crc

    @property
    def name(self) -> str:
        """Return the human-readable H4 packet-type name."""
        return {
            H4_COMMAND: "Command",
            H4_ACL: "ACL",
            H4_SCO: "SCO",
            H4_EVENT: "Event",
            H4_ISO: "ISO",
        }.get(self.packet_type, "Unknown")

    @property
    def summary(self) -> str:
        """Return a concise protocol-aware description for the traffic log."""
        if self.packet_type == H4_EVENT and len(self.h4_packet) >= 3:
            if self.rx_mode is not None:
                return "RX mode: per-byte IRQ" if self.rx_mode else "RX mode: DMA + IDLE"
            completion = self.test_completion
            if completion is not None:
                identifier, status_code, size = completion
                status = "passed" if status_code == 0 else "failed"
                return f"Test transfer {identifier} {status}, {size} bytes"
            return f"Event 0x{self.h4_packet[1]:02X}, {self.h4_packet[2]} parameter byte(s)"
        if self.packet_type == H4_COMMAND and len(self.h4_packet) >= 4:
            opcode = self.h4_packet[1] | self.h4_packet[2] << 8
            return f"Command opcode 0x{opcode:04X}"
        if self.packet_type == H4_ACL and len(self.h4_packet) >= 5:
            handle = self.h4_packet[1] | self.h4_packet[2] << 8
            return f"ACL handle/flags 0x{handle:04X}"
        return self.name

    @property
    def rx_mode(self) -> int | None:
        """Decode an `RXMD` vendor event, or return `None` for other frames."""
        if (self.packet_type == H4_EVENT and len(self.h4_packet) >= 8
                and self.h4_packet[1] == 0xFF and self.h4_packet[2] == 5
                and self.h4_packet[3:7] == b"RXMD"
                and self.h4_packet[7] in (RX_MODE_DMA, RX_MODE_BYTE_IRQ)):
            return self.h4_packet[7]
        return None

    @property
    def test_completion(self) -> tuple[int, int, int] | None:
        """Decode a `TEST` completion identifier, status, and total size."""
        if (self.packet_type == H4_EVENT and len(self.h4_packet) >= 14
                and self.h4_packet[1] == 0xFF and self.h4_packet[2] == 11
                and self.h4_packet[3:7] == b"TEST"):
            return (
                int.from_bytes(self.h4_packet[7:9], "little"),
                self.h4_packet[9],
                int.from_bytes(self.h4_packet[10:14], "little"),
            )
        return None

    @property
    def test_transfer_flags(self) -> int | None:
        """Return deterministic ACL transfer flags, or `None` for other packets."""
        info = self.test_transfer_info
        return info[3] if info is not None else None

    @property
    def test_transfer_info(self) -> tuple[int, int, int, int] | None:
        """Decode deterministic transfer total, offset, chunk size, and flags."""
        if (self.packet_type == H4_ACL and len(self.h4_packet) >= 23
                and (int.from_bytes(self.h4_packet[1:3], "little") & 0x0FFF)
                    == TEST_TRANSFER_HANDLE
                and self.h4_packet[5:9] == b"TEST" and self.h4_packet[9] == 1):
            return (
                int.from_bytes(self.h4_packet[13:17], "little"),
                int.from_bytes(self.h4_packet[17:21], "little"),
                int.from_bytes(self.h4_packet[21:23], "little"),
                self.h4_packet[10],
            )
        return None

class H4CrcStreamParser:
    """Incrementally decode H4 packets followed by the stack's two-byte CRC.

    Serial reads may contain partial frames, multiple frames, or leading corrupt
    bytes. Parser state is retained between calls to `feed()`.
    """

    _HEADER_LENGTHS = {
        H4_COMMAND: 3,
        H4_ACL: 4,
        H4_SCO: 3,
        H4_EVENT: 2,
        H4_ISO: 4,
    }

    def __init__(self) -> None:
        """Create an empty stream parser with no discarded input bytes."""
        self._buffer = bytearray()
        self.discarded_bytes = 0

    @staticmethod
    def _payload_length(packet_type: int, header: bytes) -> int:
        """Decode the payload length from a packet-type-specific HCI header.

        @throws ValueError If @p packet_type is unsupported.
        """
        if packet_type == H4_COMMAND:
            return header[2]
        if packet_type == H4_ACL:
            return header[2] | header[3] << 8
        if packet_type == H4_SCO:
            return header[2]
        if packet_type == H4_EVENT:
            return header[1]
        if packet_type == H4_ISO:
            return (header[2] | header[3] << 8) & 0x3FFF
        raise ValueError("Unsupported H4 packet type")

    def feed(self, data: bytes) -> list[ParsedFrame]:
        """Consume arbitrary serial bytes and return every complete frame.

        @param data Newly received stream bytes.
        @return Complete frames decoded during this call.
        """
        self._buffer.extend(data)
        frames: list[ParsedFrame] = []

        cursor = 0
        while cursor < len(self._buffer):
            packet_type = self._buffer[cursor]
            header_length = self._HEADER_LENGTHS.get(packet_type)
            if header_length is None:
                cursor += 1
                self.discarded_bytes += 1
                continue

            h4_header_end = cursor + 1 + header_length
            if len(self._buffer) < h4_header_end:
                break
            header = bytes(self._buffer[cursor + 1:h4_header_end])
            payload_length = self._payload_length(packet_type, header)
            if payload_length > MAX_HCI_PAYLOAD:
                cursor += 1
                self.discarded_bytes += 1
                continue

            h4_length = h4_header_end + payload_length
            framed_length = h4_length + 2
            if len(self._buffer) < framed_length:
                break

            h4_packet = bytes(self._buffer[cursor:h4_length])
            received_crc = int.from_bytes(self._buffer[h4_length:framed_length], "big")
            frames.append(
                ParsedFrame(packet_type, h4_packet, received_crc, crc16_ccitt(h4_packet))
            )
            cursor = framed_length

        if cursor != 0:
            del self._buffer[:cursor]

        return frames


COMMAND_PRESETS: dict[str, tuple[int, bytes]] = {
    "HCI Reset": (0x0C03, b""),
    "Read Local Version": (0x1001, b""),
    "Read BD_ADDR": (0x1009, b""),
    "LE Read Buffer Size": (0x2002, b""),
    "LE Enable Scan": (0x200C, bytes((0x01, 0x00))),
    "LE Disable Scan": (0x200C, bytes((0x00, 0x00))),
}
