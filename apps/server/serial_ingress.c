/** @file
 * @brief Complete-frame reassembly for the server's serial reader. */
#include "serial_ingress.h"

#include "uart_dma/ble_hci_wire.h"

#include <string.h>

/** Inspect a partial H4 header and calculate its CRC-protected frame length. */
static uint16_t h4FrameLength(const uint8_t *bytes, uint16_t available) {
    if (available < sizeof(BleHciPacketIndicator_t)) {
        return 0U;
    }
    const size_t headerLength = BleHciWire_HeaderSize(bytes[0]);
    if (headerLength == 0U) {
        return UINT16_MAX;
    }
    if ((size_t)available < headerLength) {
        return 0U;
    }
    BleHciHeader_t header;
    memcpy(&header, bytes, headerLength);
    return (uint16_t)(headerLength + BleHciWire_PayloadLength(&header)
                    + sizeof(BleHciCrcTrailer_t));
}

void ServerSerialIngress_Init(ServerSerialIngress_t *ingress) {
    memset(ingress, 0, sizeof(*ingress));
}

void ServerSerialIngress_Feed(ServerSerialIngress_t *ingress,
                              const uint8_t *bytes, size_t length,
                              ServerFrameHandler_t handler, void *context) {
    for (size_t index = 0U; index < length; index++) {
        if (ingress->length >= sizeof(ingress->bytes)) {
            ingress->length = 0U;
            ingress->expected_length = 0U;
        }
        ingress->bytes[ingress->length++] = bytes[index];
        if (ingress->expected_length == 0U) {
            const uint16_t expected = h4FrameLength(ingress->bytes, ingress->length);
            if (expected == UINT16_MAX || expected > sizeof(ingress->bytes)) {
                memmove(ingress->bytes, &ingress->bytes[1], --ingress->length);
                continue;
            }
            ingress->expected_length = expected;
        }
        if (ingress->expected_length != 0U && ingress->length == ingress->expected_length) {
            if (handler != NULL) {
                handler(ingress->bytes, ingress->length, context);
            }
            ingress->length = 0U;
            ingress->expected_length = 0U;
        }
    }
}
