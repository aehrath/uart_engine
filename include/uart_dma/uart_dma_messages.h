#ifndef UART_DMA_MESSAGES_H
#define UART_DMA_MESSAGES_H

/** @file
 * @brief Queue message representation used by the UART DMA tasks. */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/** Maximum queue payload, including the H4 header but excluding its CRC trailer. */
#define UART_DMA_MAX_PACKET_SIZE 256U

/** A fixed-capacity message transferred through FreeRTOS queues. */
typedef struct {
    /** H4 header and variable payload bytes. */
    uint8_t payload[UART_DMA_MAX_PACKET_SIZE];
    /** Number of valid bytes in `payload`. */
    uint32_t length;
} PacketMessage_t;

/** Construct a queue message from a typed packet or byte span.
 * @param payload Source object containing its packed wire representation.
 * @param length Number of bytes to copy.
 * @return Populated message, or a zero-length message for invalid arguments. */
static inline PacketMessage_t UartDma_MessageFrom(const void *payload, size_t length) {
    PacketMessage_t message = {{0U}, 0U};
    if (payload != NULL && length <= sizeof(message.payload)) {
        memcpy(message.payload, payload, length);
        message.length = (uint32_t)length;
    }
    return message;
}

#endif
