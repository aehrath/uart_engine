#ifndef APP_H4_WIRE_H
#define APP_H4_WIRE_H

/** @file
 * @brief Native-client CRC, text conversion, and framed stream parsing.
 * @ingroup app_transport */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "uart_dma/ble_hci_wire.h"

#define APP_H4_COMMAND        BLE_HCI_H4_COMMAND
#define APP_H4_ACL            BLE_HCI_H4_ACL
#define APP_H4_SCO            BLE_HCI_H4_SCO
#define APP_H4_EVENT          BLE_HCI_H4_EVENT
#define APP_H4_ISO            BLE_HCI_H4_ISO
/** Maximum CRC-protected frame size accepted by the host clients. */
#define APP_H4_MAX_FRAME_SIZE (BLE_HCI_MAX_WIRE_PACKET + sizeof(BleHciCrcTrailer_t))

/** Incremental frame parser state retained across partial serial reads. */
typedef struct {
    /** Accumulated wire bytes. */
    uint8_t bytes[APP_H4_MAX_FRAME_SIZE];
    /** Number of bytes currently accumulated. */
    size_t length;
    /** Complete frame size once its fixed header has arrived. */
    size_t expected_length;
} AppH4StreamParser_t;

/** Callback invoked for each complete CRC-protected frame.
 * @param frame H4 packet and trailing CRC.
 * @param frame_length Total bytes in @p frame.
 * @param crc_valid Whether the received CRC matches the packet bytes.
 * @param context Caller-provided context pointer. */
typedef void (*AppH4FrameHandler_t)(const uint8_t *frame, size_t frame_length,
                                    bool crc_valid, void *context);

/** Compute the host-side CRC16-CCITT. */
uint16_t AppH4_Crc16(const void *data, size_t length);
/** Parse separated hexadecimal bytes.
 * @return Parsed byte count, or `SIZE_MAX` for invalid input or overflow. */
size_t AppH4_ParseHex(const char *text, uint8_t *output, size_t capacity);
/** Format bytes as uppercase, space-separated hexadecimal text. */
void AppH4_FormatHex(char *destination, size_t capacity,
                     const void *data, size_t length);
/** Initialize a typed H4 Command packet.
 * @return Serialized H4 length, or zero for invalid arguments. */
size_t AppH4Command_Init(BleHciCommandPacket_t *packet, uint16_t opcode,
                         const uint8_t *parameters, size_t parameter_length);
/** Initialize the typed vendor command that sets or queries RX mode.
 * @param packet Destination command packet.
 * @param mode BLE_HCI_RX_MODE_DMA, BLE_HCI_RX_MODE_BYTE_IRQ, or query.
 * @return Serialized H4 command length. */
size_t AppH4RxModeCommand_Init(BleHciCommandPacket_t *packet, uint8_t mode);
/** Decode a synchronized RX-mode vendor event.
 * @param frame Complete H4 frame, optionally followed by its CRC trailer.
 * @param frame_length Available bytes in @p frame.
 * @param byte_irq Receives whether per-byte IRQ mode is authoritative.
 * @return `true` only for an `RXMD` state event. */
bool AppH4RxModeEvent_Decode(const uint8_t *frame, size_t frame_length,
                             bool *byte_irq);
/** Reset an incremental host stream parser. */
void AppH4StreamParser_Init(AppH4StreamParser_t *parser);
/** Consume serial bytes and dispatch every completed frame.
 * @return Number of frames with valid CRC values. */
size_t AppH4StreamParser_Feed(AppH4StreamParser_t *parser,
                              const uint8_t *data, size_t length,
                              AppH4FrameHandler_t handler, void *context);

#endif
