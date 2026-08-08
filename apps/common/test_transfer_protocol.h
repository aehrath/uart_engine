#ifndef TEST_TRANSFER_PROTOCOL_H
#define TEST_TRANSFER_PROTOCOL_H

/** @file
 * @brief Typed wire format for deterministic small and large test transfers.
 * @defgroup test_transfer Test transfer protocol
 * @brief Chunked ACL payload used to verify sustained UART transfers.
 * @{ */

#include <stdint.h>

#include "uart_dma/ble_hci_wire.h"

/** Reserved ACL connection handle used by test chunks. */
#define TEST_TRANSFER_HANDLE          0x0FFEU
/** Fixed bytes preceding each test chunk's generated data. */
#define TEST_TRANSFER_HEADER_SIZE     18U
/** Maximum generated data bytes carried by one ACL packet. */
#define TEST_TRANSFER_CHUNK_DATA_SIZE 230U
/** Marks the first chunk of a transfer. */
#define TEST_TRANSFER_FLAG_START      0x01U
/** Marks the final chunk of a transfer. */
#define TEST_TRANSFER_FLAG_END        0x02U
/** UI/server progress reporting interval. */
#define TEST_TRANSFER_PROGRESS_STEP   (256U * 1024U)

#if defined(_MSC_VER)
#pragma pack(push, 1)
#define TEST_TRANSFER_PACKED
#else
#define TEST_TRANSFER_PACKED __attribute__((packed))
#endif

/** Fixed metadata at the start of every test ACL payload. */
typedef struct TEST_TRANSFER_PACKED {
    /** ASCII `TEST` protocol discriminator. */
    char signature[4];
    /** Test protocol version; currently `1`. */
    uint8_t version;
    /** Bitwise combination of TEST_TRANSFER_FLAG_START and TEST_TRANSFER_FLAG_END. */
    uint8_t flags;
    /** Transfer identifier in little-endian wire order. */
    uint16_t identifier_le;
    /** Total logical transfer size in little-endian wire order. */
    uint32_t total_size_le;
    /** Offset of this chunk in little-endian wire order. */
    uint32_t offset_le;
    /** Number of valid bytes in the following data field, in little-endian order. */
    uint16_t chunk_size_le;
} TestTransferHeader_t;

/** Complete maximum-sized typed H4 ACL test packet. */
typedef struct TEST_TRANSFER_PACKED {
    /** H4 ACL header. */
    BleHciAclHeader_t h4;
    /** Test-transfer metadata. */
    TestTransferHeader_t transfer;
    /** Deterministically generated chunk data. */
    uint8_t data[TEST_TRANSFER_CHUNK_DATA_SIZE];
} TestTransferPacket_t;

#if defined(_MSC_VER)
#pragma pack(pop)
#endif

#undef TEST_TRANSFER_PACKED

_Static_assert(sizeof(TestTransferHeader_t) == TEST_TRANSFER_HEADER_SIZE,
               "Test transfer header layout changed");

/** Decode a transfer identifier. */
static inline uint16_t TestTransfer_Identifier(const TestTransferHeader_t *header) {
    return BleHciWire_FromLe16(header->identifier_le);
}

/** Decode a logical total size. */
static inline uint32_t TestTransfer_TotalSize(const TestTransferHeader_t *header) {
    return BleHciWire_FromLe32(header->total_size_le);
}

/** Decode a chunk offset. */
static inline uint32_t TestTransfer_Offset(const TestTransferHeader_t *header) {
    return BleHciWire_FromLe32(header->offset_le);
}

/** Decode a chunk data size. */
static inline uint16_t TestTransfer_ChunkSize(const TestTransferHeader_t *header) {
    return BleHciWire_FromLe16(header->chunk_size_le);
}

/** Generate the expected byte at a logical transfer offset. */
static inline uint8_t TestTransfer_PatternByte(uint32_t offset) {
    return (uint8_t)((offset * 31U + 7U) & 0xFFU);
}

/** @} */

#endif
