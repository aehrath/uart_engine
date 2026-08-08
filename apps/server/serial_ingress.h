#ifndef BLE_SERVER_SERIAL_INGRESS_H
#define BLE_SERVER_SERIAL_INGRESS_H

/** @file
 * @brief Server-side reassembly of serial reads into complete H4 frames. */

#include <stddef.h>
#include <stdint.h>

#include "uart_dma/uart_dma_messages.h"

/** Stateful ingress accumulator for fragmented or coalesced serial reads. */
typedef struct {
    /** Current frame bytes, including its CRC trailer. */
    uint8_t bytes[UART_DMA_MAX_PACKET_SIZE];
    /** Number of accumulated bytes. */
    uint16_t length;
    /** Expected complete length after the H4 header is available. */
    uint16_t expected_length;
} ServerSerialIngress_t;

/** Receives each complete frame recovered from the serial stream. */
typedef void (*ServerFrameHandler_t)(const uint8_t *frame, uint16_t length,
                                     void *context);

/** Clear the ingress accumulator. */
void ServerSerialIngress_Init(ServerSerialIngress_t *ingress);
/** Feed arbitrary serial chunks into the frame accumulator.
 * @param ingress Persistent parser state.
 * @param bytes Newly read bytes.
 * @param length Number of new bytes.
 * @param handler Complete-frame callback, or `NULL`.
 * @param context Opaque value forwarded to @p handler. */
void ServerSerialIngress_Feed(ServerSerialIngress_t *ingress,
                              const uint8_t *bytes, size_t length,
                              ServerFrameHandler_t handler, void *context);

#endif
