#ifndef UART_DMA_RING_BUFFER_H
#define UART_DMA_RING_BUFFER_H

/** @file
 * @brief Circular DMA receive-buffer ownership and extraction API. */

#include <stdbool.h>
#include <stdint.h>

/** Capacity in bytes of the circular DMA receive area. */
#define UART_DMA_RX_BUFFER_SIZE     1024U
/** Largest runtime-selectable circular receive capacity. */
#define UART_DMA_RX_BUFFER_MAX_SIZE (64U * 1024U)

/** Reset the software read cursor to the start of the DMA buffer. */
void UartDmaRingBuffer_Reset(void);
/** Select the active circular capacity and reset both cursors.
 * @return `true` for capacities from 32 bytes through the static maximum. */
bool UartDmaRingBuffer_SetCapacity(uint32_t capacity);
/** Obtain the storage address that must be programmed into RX DMA.
 * @return Mutable, cache-line-aligned receive storage. */
uint8_t *UartDma_GetRxBuffer(void);
/** Query the circular receive capacity. @return Buffer size in bytes. */
uint32_t UartDma_GetRxBufferSize(void);
/** Append one byte received through the UART RXNE interrupt path.
 * @param byte Value read from the UART receive-data register. */
void UartDmaRingBuffer_WriteByte(uint8_t byte);
/** Align the software producer cursor when switching receive paths.
 * @param write_index Next circular position to be written. */
void UartDmaRingBuffer_SetWriteIndex(uint32_t write_index);
/** Commit bytes written directly by circular DMA and retain wrap information.
 * @param write_index Next circular position to be written.
 * @param byte_count Exact bytes completed by this DMA transaction. */
void UartDmaRingBuffer_CommitDmaWrite(uint32_t write_index, uint32_t byte_count);
/** Return the software producer cursor used by per-byte IRQ mode. */
uint32_t UartDmaRingBuffer_GetWriteIndex(void);
/** Return unread bytes between the software consumer and producer cursors.
 * @param write_index Current producer position used for range validation.
 * @return Exact retained byte count, capped at the active ring capacity. */
uint32_t UartDmaRingBuffer_GetUnreadCount(uint32_t write_index);
/** Copy unread bytes through a DMA write cursor into linear storage.
 * @param write_index Current producer position derived from DMA `NDTR`.
 * @param destination Destination for newly arrived bytes.
 * @param capacity Size of @p destination.
 * @param length Receives the available byte count, including an oversized span.
 * @return `true` when nonempty data was copied; otherwise `false`. */
bool UartDmaRingBuffer_ReadTo(uint32_t write_index, uint8_t *destination,
                              uint32_t capacity, uint32_t *length);

#endif
