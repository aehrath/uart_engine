/** @file
 * @brief Cache-aligned circular RX DMA storage implementation. */
#include "uart_dma/uart_dma_ring_buffer.h"

#include <string.h>

#ifndef SCB_D_CACHE_LINE_SIZE
#define SCB_D_CACHE_LINE_SIZE 32UL
#endif

__attribute__((aligned(SCB_D_CACHE_LINE_SIZE)))
static uint8_t rxRingBuffer[UART_DMA_RX_BUFFER_MAX_SIZE];
static uint32_t lastReadIndex;
static uint32_t interruptWriteIndex;
static uint32_t activeCapacity = UART_DMA_RX_BUFFER_SIZE;
static uint32_t unreadCount;
static bool overflowed;

void UartDmaRingBuffer_Reset(void) {
    lastReadIndex = 0U;
    interruptWriteIndex = 0U;
    unreadCount = 0U;
    overflowed = false;
}

bool UartDmaRingBuffer_SetCapacity(uint32_t capacity) {
    if (capacity < 32U || capacity > UART_DMA_RX_BUFFER_MAX_SIZE) {
        return false;
    }
    activeCapacity = capacity;
    UartDmaRingBuffer_Reset();
    return true;
}

uint8_t *UartDma_GetRxBuffer(void) {
    return rxRingBuffer;
}

uint32_t UartDma_GetRxBufferSize(void) {
    return activeCapacity;
}

void UartDmaRingBuffer_WriteByte(uint8_t byte) {
    rxRingBuffer[interruptWriteIndex] = byte;
    interruptWriteIndex = (interruptWriteIndex + 1U) % activeCapacity;
    if (unreadCount < activeCapacity) {
        unreadCount++;
    } else {
        overflowed = true;
    }
}

void UartDmaRingBuffer_SetWriteIndex(uint32_t write_index) {
    if (write_index < activeCapacity) {
        interruptWriteIndex = write_index;
    }
}

void UartDmaRingBuffer_CommitDmaWrite(uint32_t write_index, uint32_t byte_count) {
    if (write_index >= activeCapacity) return;
    interruptWriteIndex = write_index;
    if (byte_count > activeCapacity - unreadCount) {
        unreadCount = activeCapacity;
        overflowed = true;
    } else {
        unreadCount += byte_count;
    }
}

uint32_t UartDmaRingBuffer_GetWriteIndex(void) {
    return interruptWriteIndex;
}

uint32_t UartDmaRingBuffer_GetUnreadCount(uint32_t write_index) {
    if (write_index >= activeCapacity) {
        return 0U;
    }
    return unreadCount;
}

bool UartDmaRingBuffer_ReadTo(uint32_t write_index, uint8_t *destination,
                              uint32_t capacity, uint32_t *length) {
    if (destination == NULL || length == NULL || write_index >= activeCapacity) {
        return false;
    }
    if (unreadCount == 0U) {
        *length = 0U;
        return false;
    }

    const uint32_t totalLength = unreadCount;
    if (overflowed || totalLength > capacity) {
        lastReadIndex = write_index;
        *length = totalLength;
        unreadCount = 0U;
        overflowed = false;
        return false;
    }

    const uint32_t firstLength = totalLength < activeCapacity - lastReadIndex
        ? totalLength : activeCapacity - lastReadIndex;
    const uint32_t secondLength = totalLength - firstLength;

    memcpy(destination, &rxRingBuffer[lastReadIndex], firstLength);
    if (secondLength != 0U) {
        memcpy(&destination[firstLength], rxRingBuffer, secondLength);
    }
    lastReadIndex = write_index;
    unreadCount = 0U;
    overflowed = false;
    *length = totalLength;
    return true;
}
