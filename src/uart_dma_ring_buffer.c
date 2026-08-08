/** @file
 * @brief Cache-aligned circular RX DMA storage implementation. */
#include "uart_dma/uart_dma_ring_buffer.h"

#include <string.h>

#ifndef SCB_D_CACHE_LINE_SIZE
#define SCB_D_CACHE_LINE_SIZE 32UL
#endif

__attribute__((aligned(SCB_D_CACHE_LINE_SIZE)))
static uint8_t rxRingBuffer[UART_DMA_RX_BUFFER_SIZE];
static uint32_t lastReadIndex;
static uint32_t interruptWriteIndex;

void UartDmaRingBuffer_Reset(void) {
    lastReadIndex = 0U;
    interruptWriteIndex = 0U;
}

uint8_t *UartDma_GetRxBuffer(void) {
    return rxRingBuffer;
}

uint32_t UartDma_GetRxBufferSize(void) {
    return UART_DMA_RX_BUFFER_SIZE;
}

void UartDmaRingBuffer_WriteByte(uint8_t byte) {
    rxRingBuffer[interruptWriteIndex] = byte;
    interruptWriteIndex = (interruptWriteIndex + 1U) % UART_DMA_RX_BUFFER_SIZE;
}

void UartDmaRingBuffer_SetWriteIndex(uint32_t write_index) {
    if (write_index < UART_DMA_RX_BUFFER_SIZE) {
        interruptWriteIndex = write_index;
    }
}

uint32_t UartDmaRingBuffer_GetWriteIndex(void) {
    return interruptWriteIndex;
}

bool UartDmaRingBuffer_ReadTo(uint32_t write_index, uint8_t *destination,
                              uint32_t capacity, uint32_t *length) {
    if (destination == NULL || length == NULL || write_index >= UART_DMA_RX_BUFFER_SIZE) {
        return false;
    }
    if (write_index == lastReadIndex) {
        *length = 0U;
        return false;
    }

    const uint32_t firstLength = write_index > lastReadIndex
        ? write_index - lastReadIndex
        : UART_DMA_RX_BUFFER_SIZE - lastReadIndex;
    const uint32_t secondLength = write_index > lastReadIndex ? 0U : write_index;
    const uint32_t totalLength = firstLength + secondLength;
    if (totalLength > capacity) {
        lastReadIndex = write_index;
        *length = totalLength;
        return false;
    }

    memcpy(destination, &rxRingBuffer[lastReadIndex], firstLength);
    if (secondLength != 0U) {
        memcpy(&destination[firstLength], rxRingBuffer, secondLength);
    }
    lastReadIndex = write_index;
    *length = totalLength;
    return true;
}
