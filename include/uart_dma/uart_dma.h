#ifndef UART_DMA_H
#define UART_DMA_H

/** @file
 * @brief Convenience umbrella and top-level FreeRTOS UART DMA API.
 * @defgroup uart_dma_core UART DMA core
 * @brief FreeRTOS queue, interrupt, CRC, and circular DMA integration.
 * @{ */

/* Convenience umbrella for applications using the complete stack. */
#include "ble_hci_parser.h"
#include "ble_hci_wire.h"
#include "uart_crc16.h"
#include "uart_dma_messages.h"
#include "uart_dma_platform.h"
#include "uart_dma_ring_buffer.h"

#include "FreeRTOS.h"

/** Allocate synchronization objects and create the producer, consumer, and TX tasks. */
void UartDmaSystem_Init(void);
/** Queue a typed H4 message for CRC framing and DMA transmission.
 * @param message Message whose length excludes the CRC trailer.
 * @param ticks_to_wait Maximum FreeRTOS queue wait duration.
 * @return `pdTRUE` when queued, otherwise `pdFALSE`. */
BaseType_t UartDma_SendPacket(const PacketMessage_t *message, TickType_t ticks_to_wait);
/** Handle UART IDLE detection and wake the RX producer task. */
void USART_IRQHandler(void);
/** Handle TX DMA completion and release the transmitter semaphore. */
void DMA_TX_Complete_IRQHandler(void);

/** @} */

#endif
