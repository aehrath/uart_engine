/**
 * @file    uart_dma_freertos.c
 * @brief   FreeRTOS task and interrupt integration for the UART DMA components.
 */

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "uart_dma/uart_dma.h"

#define UART_ISR_IDLE_Msk    (1UL << 4)
#define UART_ICR_IDLECF_Msk  (1UL << 4)

/* Static CMSIS-Core Cache Stubs (Normally resolved via core_cm7.h) */
#ifndef SCB_D_CACHE_LINE_SIZE
#define SCB_D_CACHE_LINE_SIZE 32UL
#endif

/** Host stub for the Cortex-M7 cache-invalidate primitive. */
static inline void SCB_InvalidateDCache_by_Addr(uint32_t *addr, int32_t dsize) {
    /* Hardware M7 implementation: Purges L1 Cache to read directly from physical RAM */
    (void)addr; (void)dsize;
}

/** Host stub for the Cortex-M7 cache-clean primitive. */
static inline void SCB_CleanDCache_by_Addr(uint32_t *addr, int32_t dsize) {
    /* Hardware M7 implementation: Flushes dirty registers out to physical RAM before DMA TX */
    (void)addr; (void)dsize;
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((weak))
#endif
void UartDma_PlatformTransmitDma(uint8_t *data, uint16_t size) {
    (void)data;
    (void)size;
}

/* --- System Architecture Constants --- */
#define MAX_PACKET_SIZE      UART_DMA_MAX_PACKET_SIZE
#define QUEUE_STORAGE_SIZE   16UL

/* --- Cache Line Aligned Variables --- */
/* Formatted to match 32-byte Cortex-M7 L1 cache footprints to avoid corruption */
__attribute__((aligned(SCB_D_CACHE_LINE_SIZE))) static uint8_t txActiveBuffer[MAX_PACKET_SIZE];

/* --- FreeRTOS Engine Handles --- */
static TaskHandle_t       xProducerTaskHandle = NULL;
static TaskHandle_t       xConsumerTaskHandle = NULL;
static TaskHandle_t       xTxEngineTaskHandle = NULL;

static QueueHandle_t      xRxPacketQueueHandle = NULL;
static QueueHandle_t      xTxPacketQueueHandle = NULL;
static SemaphoreHandle_t  xTxDmaSemaphore      = NULL;

/* --- Priority Scheduling Matrix --- */
#define PRIORITY_PRODUCER_TASK  (configMAX_PRIORITIES - 1) /* Critical Hardware Layer */
#define PRIORITY_TX_ENGINE_TASK (configMAX_PRIORITIES - 2) /* High Priority Outbound Pipeline */
#define PRIORITY_CONSUMER_TASK  (tskIDLE_PRIORITY + 1)     /* Standard Application Processing */

/* --- Task Prototypes --- */
/** Convert unread circular DMA bytes into validated queue messages. */
void vProducerTask(void *pvParameters);
/** Feed validated queue messages into the Bluetooth HCI parser. */
void vConsumerTask(void *pvParameters);
/** Append CRC trailers and serialize queued DMA transmissions. */
void vTxEngineTask(void *pvParameters);

void UartDmaSystem_Init(void) {
    UartDmaRingBuffer_Reset();
    /* 1. Allocate Inter-Process Communication Pipelines */
    xRxPacketQueueHandle = xQueueCreate(QUEUE_STORAGE_SIZE, sizeof(PacketMessage_t));
    xTxPacketQueueHandle = xQueueCreate(QUEUE_STORAGE_SIZE, sizeof(PacketMessage_t));

    /* 2. Instantiate Hardware-to-Task Sync Flags */
    xTxDmaSemaphore = xSemaphoreCreateBinary();
    if (xTxDmaSemaphore != NULL) {
        xSemaphoreGive(xTxDmaSemaphore); /* Initialize as available */
    }

    if (xRxPacketQueueHandle == NULL || xTxPacketQueueHandle == NULL || xTxDmaSemaphore == NULL) {
        while(1); /* System Out Of Memory Catch */
    }

    /* 3. Launch Core Application Sub-Tasks */
    xTaskCreate(vProducerTask, "Prod_RX_Task", configMINIMAL_STACK_SIZE + 256, NULL, PRIORITY_PRODUCER_TASK, &xProducerTaskHandle);
    xTaskCreate(vTxEngineTask, "Tx_Eng_Task",  configMINIMAL_STACK_SIZE + 256, NULL, PRIORITY_TX_ENGINE_TASK, &xTxEngineTaskHandle);
    xTaskCreate(vConsumerTask, "Cons_App_Task", configMINIMAL_STACK_SIZE + 512, NULL, PRIORITY_CONSUMER_TASK, &xConsumerTaskHandle);
}

void USART_IRQHandler(void) {
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    /* Detect IDLE Line State Trigger (Quiet Line after Packet Segment) */
    if ((USART_HW->ISR & UART_ISR_IDLE_Msk) != 0) {
        USART_HW->ICR = UART_ICR_IDLECF_Msk; /* Clear flag immediately */

        if (xProducerTaskHandle != NULL) {
            vTaskNotifyGiveFromISR(xProducerTaskHandle, &xHigherPriorityTaskWoken);
        }
    }

    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

void DMA_TX_Complete_IRQHandler(void) {
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    /* Clear hardware DMA transfer complete flags here depending on chip vendor specs */

    if (xTxDmaSemaphore != NULL) {
        xSemaphoreGiveFromISR(xTxDmaSemaphore, &xHigherPriorityTaskWoken);
    }

    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

/** Convert each UART-IDLE-delimited DMA span into a CRC-validated RX message.
 * @param pvParameters Unused FreeRTOS task context. */
void vProducerTask(void *pvParameters) {
    (void)pvParameters;

    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        /* Purge CPU Data L1 Cache Line Registers to match True Physical Data in RAM */
        SCB_InvalidateDCache_by_Addr((uint32_t *)UartDma_GetRxBuffer(),
                                    (int32_t)UartDma_GetRxBufferSize());

        uint32_t currentNDTR = DMA_RX_STREAM->NDTR;
        uint32_t currentWriteIndex = UartDma_GetRxBufferSize() - currentNDTR;

        PacketMessage_t newPacket;
        uint32_t dataLength = 0;
        const bool validLayout = UartDmaRingBuffer_ReadTo(
            currentWriteIndex, newPacket.payload, sizeof(newPacket.payload), &dataLength);
        newPacket.length = dataLength;

        /* Core Framing Integrity Verification (CRC16 check on last 2 trailing payload bytes) */
        if (validLayout && dataLength > sizeof(BleHciCrcTrailer_t)) {
            const uint32_t payloadLength = dataLength - sizeof(BleHciCrcTrailer_t);
            BleHciCrcTrailer_t trailer;
            memcpy(&trailer, &newPacket.payload[payloadLength], sizeof(trailer));
            const uint16_t receivedCRC = BleHciWire_FromBe16(trailer.value_be);
            const uint16_t computedCRC = UartDma_ComputeCrc16(
                newPacket.payload, payloadLength);

            if (computedCRC == receivedCRC) {
                newPacket.length = payloadLength; /* Strip CRC field for Application consumer convenience */
                xQueueSend(xRxPacketQueueHandle, &newPacket, 0U);
            } else {
                /* Drop packet due to Frame Corruption */
            }
        }
    }
}

/** Deliver validated RX message bytes to the Bluetooth HCI parser.
 * @param pvParameters Unused FreeRTOS task context. */
void vConsumerTask(void *pvParameters) {
    (void)pvParameters;
    PacketMessage_t receivedPacket;

    while (1) {
        /* Blocks natively with zero CPU overhead until the DMA Producer pushes a data frame */
        if (xQueueReceive(xRxPacketQueueHandle, &receivedPacket, portMAX_DELAY) == pdTRUE) {

            /* Iterate cleanly through the thread-isolated local data block array */
            for (uint32_t i = 0; i < receivedPacket.length; i++) {

                /* Feed the incoming data stream into the H4 decoding state engine */
                BleHci_ParseByte(receivedPacket.payload[i]);
            }
        }
    }
}

BaseType_t UartDma_SendPacket(const PacketMessage_t *message, TickType_t ticks_to_wait) {
    if (message == NULL || message->length > (MAX_PACKET_SIZE - 2) || xTxPacketQueueHandle == NULL) {
        return pdFALSE;
    }
    return xQueueSend(xTxPacketQueueHandle, message, ticks_to_wait);
}

/** Append CRC trailers and serialize outbound messages through the DMA engine.
 * @param pvParameters Unused FreeRTOS task context. */
void vTxEngineTask(void *pvParameters) {
    (void)pvParameters;
    PacketMessage_t txMsg;

    while (1) {
        if (xQueueReceive(xTxPacketQueueHandle, &txMsg, portMAX_DELAY) == pdTRUE) {

            /* Secure the unique transmission hardware locking token */
            if (xSemaphoreTake(xTxDmaSemaphore, portMAX_DELAY) == pdTRUE) {

                /* Assemble final output framework container */
                memcpy(txActiveBuffer, txMsg.payload, txMsg.length);

                /* Compute and attach outbound trailing framing validation fields */
                const uint16_t packetCRC = UartDma_ComputeCrc16(txActiveBuffer, txMsg.length);
                const BleHciCrcTrailer_t trailer = {BleHciWire_ToBe16(packetCRC)};
                memcpy(&txActiveBuffer[txMsg.length], &trailer, sizeof(trailer));

                const uint32_t totalTransmitSize = txMsg.length + sizeof(trailer);

                /* Push dirty CPU L1 cache contents to physical memory before starting DMA */
                SCB_CleanDCache_by_Addr((uint32_t *)txActiveBuffer, MAX_PACKET_SIZE);

                /* Fire asynchronous Non-blocking DMA burst */
                UartDma_PlatformTransmitDma(txActiveBuffer, (uint16_t)totalTransmitSize);

                /* Note: The xTxDmaSemaphore is unlocked by the DMA_TX_Complete_IRQHandler */
            }
        }
    }
}
