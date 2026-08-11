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

/** Selectable UART receive implementation. */
typedef enum {
    /** Circular DMA receives bytes and UART IDLE wakes the producer. */
    UART_DMA_RX_MODE_DMA_IDLE = 0,
    /** Every RX byte enters through the UART RXNE interrupt. */
    UART_DMA_RX_MODE_BYTE_IRQ = 1
} UartDmaRxMode_t;

/** Randomized competing-task configuration for full-system load simulation. */
typedef struct {
    /** Random-range weight from 0 through 100. */
    uint32_t weightPercent;
    /** Minimum simulated target CPU cycles in one busy burst. */
    uint32_t minimumCycles;
    /** Maximum simulated target CPU cycles; zero disables competing work. */
    uint32_t maximumCycles;
    /** Target CPU clock used to convert cycles into elapsed time. */
    uint32_t systemClockHz;
} UartDmaThreadLoadConfig_t;

/** Cumulative measurements from the randomized competing task. */
typedef struct {
    /** Most recently accepted configuration. */
    UartDmaThreadLoadConfig_t config;
    /** Random scheduling decisions evaluated since initialization. */
    uint64_t decisions;
    /** Decisions that produced a busy burst. */
    uint64_t activations;
    /** Sum of configured target cycles across all bursts. */
    uint64_t totalCycles;
    /** Measured wall time spent executing busy bursts. */
    uint64_t totalBusyUs;
    /** Target cycles selected for the most recent burst. */
    uint32_t lastCycles;
    /** Whether the competing task is currently inside a busy burst. */
    bool active;
} UartDmaThreadLoadMetrics_t;

/** Snapshot of measured work crossing the RX producer/consumer boundary. */
typedef struct {
    /** CRC-valid bytes successfully enqueued by the producer. */
    uint64_t producedBytes;
    /** Bytes removed from the RX queue by the consumer. */
    uint64_t consumedBytes;
    /** CRC-valid packets successfully enqueued by the producer. */
    uint32_t producedPackets;
    /** Packets removed by the consumer. */
    uint32_t consumedPackets;
    /** Frames discarded because their CRC failed. */
    uint32_t crcDrops;
    /** Frames discarded because the RX queue was full. */
    uint32_t queueDrops;
    /** Ring spans that could not fit in one PacketMessage. */
    uint32_t ringDrops;
    /** Exact payload bytes currently retained by the RX queue. */
    uint32_t queuedBytes;
    /** Packet slots currently occupied in the RX queue. */
    uint32_t queueDepth;
    /** Fixed packet capacity of the RX queue. */
    uint32_t queueCapacity;
    /** Largest observed packet depth since the previous metrics snapshot. */
    uint32_t queueHighWater;
    /** Bytes currently awaiting the producer. */
    uint32_t ringBurstBytes;
    /** Largest producer wake-up span since the previous metrics snapshot. */
    uint32_t ringHighWaterBytes;
    /** Active circular DMA capacity. */
    uint32_t ringCapacity;
    /** Most recent enqueue-to-consume latency. */
    uint64_t lastLatencyUs;
    /** Largest observed enqueue-to-consume latency. */
    uint64_t maxLatencyUs;
} UartDmaPipelineMetrics_t;

/** Allocate synchronization objects and create the producer, consumer, and TX tasks. */
void UartDmaSystem_Init(void);
/** Queue a typed H4 message for CRC framing and DMA transmission.
 * @param message Message whose length excludes the CRC trailer.
 * @param ticks_to_wait Maximum FreeRTOS queue wait duration.
 * @return `pdTRUE` when queued, otherwise `pdFALSE`. */
BaseType_t UartDma_SendPacket(const PacketMessage_t *message, TickType_t ticks_to_wait);
/** Switch the receive implementation between circular DMA and per-byte IRQ.
 * Switching is intended at a complete frame boundary.
 * @param mode New receive implementation. */
void UartDma_SetRxMode(UartDmaRxMode_t mode);
/** Return the currently selected receive implementation. */
UartDmaRxMode_t UartDma_GetRxMode(void);
/** Return receiver IRQ calls: RXNE calls in byte mode or DMA completions in DMA mode. */
uint32_t UartDma_GetRxInterruptCount(void);
/** Change the active circular RX ring capacity at a frame boundary. */
bool UartDma_SetRxBufferSize(uint32_t size);
/** Copy a current snapshot of measured RX pipeline telemetry. */
void UartDma_GetPipelineMetrics(UartDmaPipelineMetrics_t *metrics);
/** Restore the disabled randomized-load configuration. */
void UartDmaThreadLoad_DefaultConfig(UartDmaThreadLoadConfig_t *config);
/** Validate and atomically apply randomized competing-task settings.
 * @return `true` when every field is within the supported range. */
bool UartDmaThreadLoad_Configure(const UartDmaThreadLoadConfig_t *config);
/** Copy the current load configuration and cumulative measurements. */
void UartDmaThreadLoad_GetMetrics(UartDmaThreadLoadMetrics_t *metrics);
/** Platform-overridable monotonic microsecond clock used by telemetry. */
uint64_t UartDma_PlatformTimeUs(void);
/** Store RXNE bytes in IRQ mode and wake the producer on UART IDLE. */
void USART_IRQHandler(void);
/** Record one completed RX DMA transaction in DMA receive mode. */
void DMA_RX_Complete_IRQHandler(void);
/** Handle TX DMA completion and release the transmitter semaphore. */
void DMA_TX_Complete_IRQHandler(void);

/** @} */

#endif
