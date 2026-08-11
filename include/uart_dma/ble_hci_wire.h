#ifndef BLE_HCI_WIRE_H
#define BLE_HCI_WIRE_H

/** @file
 * @brief Packed Bluetooth HCI H4 wire structures and endian helpers.
 * @ingroup ble_hci_protocol */

#include <stddef.h>
#include <stdint.h>

/** @name H4 packet indicators */
/** @{ */
#define BLE_HCI_H4_COMMAND 0x01U
#define BLE_HCI_H4_ACL     0x02U
#define BLE_HCI_H4_SCO     0x03U
#define BLE_HCI_H4_EVENT   0x04U
#define BLE_HCI_H4_ISO     0x05U
/** @} */

/** Vendor command used to set or query the UART receive mode. */
#define BLE_HCI_RX_MODE_OPCODE       0xFC01U
/** Vendor command configuring randomized competing-task load. */
#define BLE_HCI_THREAD_LOAD_OPCODE   0xFC02U
/** Vendor event code carrying synchronized application state. */
#define BLE_HCI_VENDOR_EVENT_CODE    0xFFU
/** DMA circular receive mode value used on the control wire. */
#define BLE_HCI_RX_MODE_DMA          0x00U
/** Per-byte UART interrupt receive mode value used on the control wire. */
#define BLE_HCI_RX_MODE_BYTE_IRQ     0x01U
/** Query-only value that leaves the current receive mode unchanged. */
#define BLE_HCI_RX_MODE_QUERY        0xFFU

/** Maximum parameter bytes encoded by an HCI Command header. */
#define BLE_HCI_MAX_COMMAND_PARAMETERS 255U
/** Maximum H4 bytes before the project-specific CRC trailer. */
#define BLE_HCI_MAX_WIRE_PACKET        4094U
/** Maximum ACL payload fitting in BLE_HCI_MAX_WIRE_PACKET. */
#define BLE_HCI_MAX_ACL_DATA           (BLE_HCI_MAX_WIRE_PACKET - 5U)

/** Reverse the byte order of a 16-bit integer. */
static inline uint16_t BleHciWire_ByteSwap16(uint16_t value) {
    return (uint16_t)((value << 8) | (value >> 8));
}

/** Reverse the byte order of a 32-bit integer. */
static inline uint32_t BleHciWire_ByteSwap32(uint32_t value) {
    return ((value & 0x000000FFUL) << 24)
         | ((value & 0x0000FF00UL) << 8)
         | ((value & 0x00FF0000UL) >> 8)
         | ((value & 0xFF000000UL) >> 24);
}

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
/** Convert host-order 16-bit data to little-endian wire order. */
static inline uint16_t BleHciWire_ToLe16(uint16_t value) { return BleHciWire_ByteSwap16(value); }
/** Convert host-order 32-bit data to little-endian wire order. */
static inline uint32_t BleHciWire_ToLe32(uint32_t value) { return BleHciWire_ByteSwap32(value); }
/** Decode a little-endian 16-bit wire field. */
static inline uint16_t BleHciWire_FromLe16(uint16_t value) { return BleHciWire_ByteSwap16(value); }
/** Decode a little-endian 32-bit wire field. */
static inline uint32_t BleHciWire_FromLe32(uint32_t value) { return BleHciWire_ByteSwap32(value); }
/** Convert host-order CRC data to big-endian wire order. */
static inline uint16_t BleHciWire_ToBe16(uint16_t value) { return value; }
/** Decode a big-endian 16-bit CRC field. */
static inline uint16_t BleHciWire_FromBe16(uint16_t value) { return value; }
#else
/** Convert host-order 16-bit data to little-endian wire order. */
static inline uint16_t BleHciWire_ToLe16(uint16_t value) { return value; }
/** Convert host-order 32-bit data to little-endian wire order. */
static inline uint32_t BleHciWire_ToLe32(uint32_t value) { return value; }
/** Decode a little-endian 16-bit wire field. */
static inline uint16_t BleHciWire_FromLe16(uint16_t value) { return value; }
/** Decode a little-endian 32-bit wire field. */
static inline uint32_t BleHciWire_FromLe32(uint32_t value) { return value; }
/** Convert host-order CRC data to big-endian wire order. */
static inline uint16_t BleHciWire_ToBe16(uint16_t value) { return BleHciWire_ByteSwap16(value); }
/** Decode a big-endian 16-bit CRC field. */
static inline uint16_t BleHciWire_FromBe16(uint16_t value) { return BleHciWire_ByteSwap16(value); }
#endif

#if defined(_MSC_VER)
#pragma pack(push, 1)
#define BLE_HCI_WIRE_PACKED
#else
#define BLE_HCI_WIRE_PACKED __attribute__((packed))
#endif

/** Packed H4 packet indicator shared by every wire header. */
typedef struct BLE_HCI_WIRE_PACKED {
    /** One of the BLE_HCI_H4_* constants. */
    uint8_t value;
} BleHciPacketIndicator_t;

/** Command fields following the packet indicator. */
typedef struct BLE_HCI_WIRE_PACKED {
    /** Command opcode in little-endian wire order. */
    uint16_t opcode_le;
    /** Number of command parameter bytes that follow. */
    uint8_t parameter_length;
} BleHciCommandFields_t;

/** Packed four-byte H4 Command header. */
typedef struct BLE_HCI_WIRE_PACKED {
    /** Typed H4 packet indicator. */
    BleHciPacketIndicator_t indicator;
    /** Command-specific fixed fields. */
    BleHciCommandFields_t fields;
} BleHciCommandHeader_t;

/** ACL fields following the packet indicator. */
typedef struct BLE_HCI_WIRE_PACKED {
    /** Connection handle and ACL flags in little-endian wire order. */
    uint16_t handle_flags_le;
    /** ACL payload length in little-endian wire order. */
    uint16_t data_length_le;
} BleHciAclFields_t;

/** Packed five-byte H4 ACL Data header. */
typedef struct BLE_HCI_WIRE_PACKED {
    /** Typed H4 packet indicator. */
    BleHciPacketIndicator_t indicator;
    /** ACL-specific fixed fields. */
    BleHciAclFields_t fields;
} BleHciAclHeader_t;

/** Event fields following the packet indicator. */
typedef struct BLE_HCI_WIRE_PACKED {
    /** Bluetooth event code. */
    uint8_t event_code;
    /** Number of event parameter bytes that follow. */
    uint8_t parameter_length;
} BleHciEventFields_t;

/** Packed three-byte H4 Event header. */
typedef struct BLE_HCI_WIRE_PACKED {
    /** Typed H4 packet indicator. */
    BleHciPacketIndicator_t indicator;
    /** Event-specific fixed fields. */
    BleHciEventFields_t fields;
} BleHciEventHeader_t;

/** Synchronous-data fields following the packet indicator. */
typedef struct BLE_HCI_WIRE_PACKED {
    /** Connection handle and flags in little-endian wire order. */
    uint16_t handle_flags_le;
    /** Synchronous payload length. */
    uint8_t data_length;
} BleHciScoFields_t;

/** Packed four-byte H4 synchronous-data header. */
typedef struct BLE_HCI_WIRE_PACKED {
    /** Typed H4 packet indicator. */
    BleHciPacketIndicator_t indicator;
    /** SCO-specific fixed fields. */
    BleHciScoFields_t fields;
} BleHciScoHeader_t;

/** Isochronous-data fields following the packet indicator. */
typedef struct BLE_HCI_WIRE_PACKED {
    /** Connection handle and ISO flags in little-endian wire order. */
    uint16_t handle_flags_le;
    /** ISO payload length and status bits in little-endian wire order. */
    uint16_t data_length_le;
} BleHciIsoFields_t;

/** Packed five-byte H4 isochronous-data header. */
typedef struct BLE_HCI_WIRE_PACKED {
    /** Typed H4 packet indicator. */
    BleHciPacketIndicator_t indicator;
    /** ISO-specific fixed fields. */
    BleHciIsoFields_t fields;
} BleHciIsoHeader_t;

/** Typed storage for any supported fixed H4 header. */
typedef union {
    /** Raw storage used while receiving a header incrementally. */
    uint8_t bytes[sizeof(BleHciIsoHeader_t)];
    /** H4 Command header view. */
    BleHciCommandHeader_t command;
    /** H4 ACL Data header view. */
    BleHciAclHeader_t acl;
    /** H4 synchronous-data header view. */
    BleHciScoHeader_t sco;
    /** H4 Event header view. */
    BleHciEventHeader_t event;
    /** H4 isochronous-data header view. */
    BleHciIsoHeader_t iso;
} BleHciHeader_t;

/** Maximum-capacity typed H4 Command packet. */
typedef struct BLE_HCI_WIRE_PACKED {
    /** Fixed H4 Command header. */
    BleHciCommandHeader_t header;
    /** Valid prefix selected by `header.fields.parameter_length`. */
    uint8_t parameters[BLE_HCI_MAX_COMMAND_PARAMETERS];
} BleHciCommandPacket_t;

/** Maximum-capacity typed H4 ACL packet. */
typedef struct BLE_HCI_WIRE_PACKED {
    /** Fixed H4 ACL header. */
    BleHciAclHeader_t header;
    /** Valid prefix selected by `header.fields.data_length_le`. */
    uint8_t data[BLE_HCI_MAX_ACL_DATA];
} BleHciAclPacket_t;

/** Parameters carried by a standard HCI Command Complete event. */
typedef struct BLE_HCI_WIRE_PACKED {
    /** Controller command-credit count. */
    uint8_t command_packets;
    /** Completed opcode in little-endian wire order. */
    uint16_t opcode_le;
    /** HCI completion status. */
    uint8_t status;
} BleHciCommandCompleteParameters_t;

/** Standard HCI Command Complete event with one status byte. */
typedef struct BLE_HCI_WIRE_PACKED {
    /** Event header; event code is `0x0e`. */
    BleHciEventHeader_t header;
    /** Typed Command Complete parameters. */
    BleHciCommandCompleteParameters_t parameters;
} BleHciCommandCompleteEvent_t;

/** Parameters carried by the vendor test-completion event. */
typedef struct BLE_HCI_WIRE_PACKED {
    /** ASCII `TEST` discriminator. */
    char signature[4];
    /** Transfer identifier in little-endian wire order. */
    uint16_t identifier_le;
    /** Zero for verified success, nonzero for failure. */
    uint8_t status;
    /** Verified transfer size in little-endian wire order. */
    uint32_t total_size_le;
} BleHciTestCompleteParameters_t;

/** Vendor event acknowledging a completed deterministic test transfer. */
typedef struct BLE_HCI_WIRE_PACKED {
    /** Vendor-specific HCI Event header. */
    BleHciEventHeader_t header;
    /** Typed test-completion parameters. */
    BleHciTestCompleteParameters_t parameters;
} BleHciTestCompleteEvent_t;

/** Parameter carried by the vendor RX-mode command. */
typedef struct BLE_HCI_WIRE_PACKED {
    /** BLE_HCI_RX_MODE_DMA, BLE_HCI_RX_MODE_BYTE_IRQ, or BLE_HCI_RX_MODE_QUERY. */
    uint8_t mode;
} BleHciRxModeCommandParameters_t;

/** Parameters carried by the randomized competing-task command. */
typedef struct BLE_HCI_WIRE_PACKED {
    /** ASCII `LOAD` discriminator. */
    char signature[4];
    /** Random-range weight from 0 through 100. */
    uint8_t weight_percent;
    /** Inclusive minimum target CPU cycles in little-endian order. */
    uint32_t minimum_cycles_le;
    /** Inclusive maximum cycles; zero disables competing work. */
    uint32_t maximum_cycles_le;
} BleHciThreadLoadCommandParameters_t;

/** Parameters carried by a synchronized RX-mode vendor event. */
typedef struct BLE_HCI_WIRE_PACKED {
    /** ASCII `RXMD` discriminator. */
    char signature[4];
    /** Current BLE_HCI_RX_MODE_* value. */
    uint8_t mode;
} BleHciRxModeEventParameters_t;

/** Vendor event broadcasting the server's current UART receive mode. */
typedef struct BLE_HCI_WIRE_PACKED {
    /** Vendor-specific HCI Event header. */
    BleHciEventHeader_t header;
    /** Typed synchronized mode state. */
    BleHciRxModeEventParameters_t parameters;
} BleHciRxModeEvent_t;

/** Project-specific big-endian CRC trailer appended after an H4 packet. */
typedef struct BLE_HCI_WIRE_PACKED {
    /** CRC16-CCITT in big-endian wire order. */
    uint16_t value_be;
} BleHciCrcTrailer_t;

#if defined(_MSC_VER)
#pragma pack(pop)
#endif

#undef BLE_HCI_WIRE_PACKED

_Static_assert(sizeof(BleHciCommandHeader_t) == 4U, "H4 command header layout changed");
_Static_assert(sizeof(BleHciPacketIndicator_t) == 1U, "H4 indicator layout changed");
_Static_assert(sizeof(BleHciCommandFields_t) == 3U, "H4 command fields layout changed");
_Static_assert(sizeof(BleHciAclFields_t) == 4U, "H4 ACL fields layout changed");
_Static_assert(sizeof(BleHciEventFields_t) == 2U, "H4 event fields layout changed");
_Static_assert(sizeof(BleHciScoFields_t) == 3U, "H4 SCO fields layout changed");
_Static_assert(sizeof(BleHciIsoFields_t) == 4U, "H4 ISO fields layout changed");
_Static_assert(sizeof(BleHciCommandCompleteParameters_t) == 4U,
               "Command Complete parameters layout changed");
_Static_assert(sizeof(BleHciTestCompleteParameters_t) == 11U,
               "Test Complete parameters layout changed");
_Static_assert(sizeof(BleHciRxModeCommandParameters_t) == 1U,
               "RX mode command parameters layout changed");
_Static_assert(sizeof(BleHciThreadLoadCommandParameters_t) == 13U,
               "Thread-load command parameters layout changed");
_Static_assert(sizeof(BleHciRxModeEventParameters_t) == 5U,
               "RX mode event parameters layout changed");
_Static_assert(sizeof(BleHciAclHeader_t) == 5U, "H4 ACL header layout changed");
_Static_assert(sizeof(BleHciEventHeader_t) == 3U, "H4 event header layout changed");
_Static_assert(sizeof(BleHciScoHeader_t) == 4U, "H4 SCO header layout changed");
_Static_assert(sizeof(BleHciIsoHeader_t) == 5U, "H4 ISO header layout changed");
_Static_assert(sizeof(BleHciCommandCompleteEvent_t) == 7U, "Command Complete layout changed");
_Static_assert(sizeof(BleHciTestCompleteEvent_t) == 14U, "Test Complete layout changed");
_Static_assert(sizeof(BleHciRxModeEvent_t) == 8U, "RX mode event layout changed");
_Static_assert(sizeof(BleHciCrcTrailer_t) == 2U, "CRC trailer layout changed");

/** Return the fixed header size for an H4 packet indicator.
 * @param indicator One of the BLE_HCI_H4_* values.
 * @return Typed header size, or zero for an unknown indicator. */
size_t BleHciWire_HeaderSize(uint8_t indicator);

/** Read the payload length from a complete typed H4 header.
 * @param header Header whose indicator selects its typed union member.
 * @return Payload length in host byte order. */
uint16_t BleHciWire_PayloadLength(const BleHciHeader_t *header);

#endif
