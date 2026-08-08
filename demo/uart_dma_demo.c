/** @file
 * @brief Headless FreeRTOS round-trip demo for UART TX DMA and circular RX DMA. */
#include "uart_dma/uart_dma.h"

#include "queue.h"
#include "task.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define UART_IDLE_FLAG      (1UL << 4)
#define DEMO_ROUND_TRIPS    4U
#define WIRE_QUEUE_DEPTH    4U

/** Queue item representing bytes observed by the simulated controller. */
typedef struct {
    /** CRC-protected wire bytes. */
    uint8_t bytes[UART_DMA_MAX_PACKET_SIZE];
    /** Valid byte count. */
    uint16_t length;
} WireFrame_t;

static UART_TypeDef uartRegisters;
static DMA_Stream_TypeDef rxDmaRegisters;
static DMA_Stream_TypeDef txDmaRegisters;

UART_TypeDef *USART_HW = &uartRegisters;
DMA_Stream_TypeDef *DMA_RX_STREAM = &rxDmaRegisters;
DMA_Stream_TypeDef *DMA_TX_STREAM = &txDmaRegisters;

static QueueHandle_t controllerRxQueue;
static TaskHandle_t applicationTaskHandle;
static uint32_t dmaWriteIndex;
static uint32_t completedRoundTrips;

/** Copy one typed object or byte span into the circular receive DMA area. */
static void copyToDmaRing(const void *source, uint16_t length) {
    const uint8_t *const bytes = source;
    uint8_t *const dmaBuffer = UartDma_GetRxBuffer();
    const uint32_t dmaBufferSize = UartDma_GetRxBufferSize();
    for (uint16_t index = 0U; index < length; index++) {
        dmaBuffer[dmaWriteIndex] = bytes[index];
        dmaWriteIndex = (dmaWriteIndex + 1U) % dmaBufferSize;
    }
}

/** Append a CRC trailer, inject a controller response, and raise UART IDLE. */
static void injectControllerFrame(const void *payload, uint16_t payloadLength) {
    const uint16_t crc = UartDma_ComputeCrc16(payload, payloadLength);
    const uint32_t dmaBufferSize = UartDma_GetRxBufferSize();
    const BleHciCrcTrailer_t trailer = {BleHciWire_ToBe16(crc)};

    copyToDmaRing(payload, payloadLength);
    copyToDmaRing(&trailer, sizeof(trailer));

    DMA_RX_STREAM->NDTR = dmaBufferSize - dmaWriteIndex;
    USART_HW->ISR = UART_IDLE_FLAG;
    USART_IRQHandler();
    USART_HW->ISR = 0U;
}

/* Platform hook called by the library's TX engine when a DMA transfer starts. */
/** Simulated TX DMA hook that forwards frames to the controller task. */
void UartDma_PlatformTransmitDma(uint8_t *data, uint16_t size) {
    WireFrame_t frame = {{0U}, size};

    if (size <= sizeof(frame.bytes)) {
        memcpy(frame.bytes, data, size);
        (void)xQueueSend(controllerRxQueue, &frame, 0U);
    }

    DMA_TX_Complete_IRQHandler();
}

/** Count Command Complete events and wake the application task. */
static void onHciEvent(uint8_t eventCode, uint8_t parameterLength,
                       const uint8_t *parameters) {
    if ((eventCode == 0x0EU) &&
        (parameterLength == sizeof(BleHciCommandCompleteParameters_t))) {
        BleHciCommandCompleteParameters_t completion;
        memcpy(&completion, parameters, sizeof(completion));
        const uint16_t opcode = BleHciWire_FromLe16(completion.opcode_le);

        completedRoundTrips++;
        printf("APP <- UART DMA: Command Complete opcode=0x%04X status=%u (%u/%u)\n",
               opcode, completion.status, completedRoundTrips, DEMO_ROUND_TRIPS);
        (void)xTaskNotifyGive(applicationTaskHandle);
    }
}

/** Simulate a BLE controller that validates commands and returns completion events. */
static void controllerTask(void *parameters) {
    (void)parameters;
    WireFrame_t command;

    for (;;) {
        if (xQueueReceive(controllerRxQueue, &command, portMAX_DELAY) == pdTRUE) {
            if (command.length < 6U) {
                continue;
            }

            const uint16_t payloadLength = (uint16_t)(command.length - 2U);
            BleHciCrcTrailer_t trailer;
            memcpy(&trailer, &command.bytes[payloadLength], sizeof(trailer));
            const uint16_t receivedCrc = BleHciWire_FromBe16(trailer.value_be);
            const uint16_t computedCrc = UartDma_ComputeCrc16(command.bytes, payloadLength);

            BleHciCommandHeader_t commandHeader;
            memcpy(&commandHeader, command.bytes, sizeof(commandHeader));
            if ((receivedCrc == computedCrc) &&
                (commandHeader.indicator.value == BLE_HCI_H4_COMMAND)) {
                const uint16_t opcode = BleHciWire_FromLe16(
                    commandHeader.fields.opcode_le);
                const BleHciCommandCompleteEvent_t response = {
                    .header = {
                        .indicator = {.value = BLE_HCI_H4_EVENT},
                        .fields = {
                            .event_code = 0x0EU,
                            .parameter_length = 4U,
                        },
                    },
                    .parameters = {
                        .command_packets = 1U,
                        .opcode_le = commandHeader.fields.opcode_le,
                        .status = 0U,
                    },
                };

                printf("CTRL <- TX DMA: HCI command opcode=0x%04X, CRC valid\n", opcode);
                printf("CTRL -> RX DMA: Command Complete opcode=0x%04X\n", opcode);
                injectControllerFrame((const uint8_t *)&response,
                                      (uint16_t)sizeof(response));
            }
        }
    }
}

/** Send the demo's command sequence and wait for every controller response. */
static void applicationTask(void *parameters) {
    (void)parameters;
    static const uint16_t opcodes[DEMO_ROUND_TRIPS] = {
        0x0C03U, /* HCI Reset */
        0x1001U, /* Read Local Version Information */
        0x1009U, /* Read BD_ADDR */
        0x2002U  /* LE Read Buffer Size */
    };

    for (uint32_t index = 0U; index < DEMO_ROUND_TRIPS; index++) {
        const BleHciCommandHeader_t hciCommand = {
            .indicator = {.value = BLE_HCI_H4_COMMAND},
            .fields = {
                .opcode_le = BleHciWire_ToLe16(opcodes[index]),
                .parameter_length = 0U,
            },
        };
        PacketMessage_t command = UartDma_MessageFrom(&hciCommand, sizeof(hciCommand));

        printf("APP -> UART DMA: HCI command opcode=0x%04X\n", opcodes[index]);
        if (UartDma_SendPacket(&command, pdMS_TO_TICKS(100U)) != pdTRUE) {
            puts("Failed to enqueue HCI command.");
            vTaskEndScheduler();
            return;
        }

        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000U)) == 0U) {
            puts("Timed out waiting for the controller response.");
            vTaskEndScheduler();
            return;
        }
    }

    puts("Demo completed: all messages traversed TX DMA, controller, RX DMA, and HCI parser.");
    vTaskEndScheduler();
}

/** Create the simulation resources and start the FreeRTOS scheduler. */
int main(void) {
    controllerRxQueue = xQueueCreate(WIRE_QUEUE_DEPTH, sizeof(WireFrame_t));
    if (controllerRxQueue == NULL) {
        return 1;
    }

    DMA_RX_STREAM->NDTR = UartDma_GetRxBufferSize();
    BleHci_SetEventHandler(onHciEvent);
    UartDmaSystem_Init();

    if (xTaskCreate(controllerTask, "Controller", configMINIMAL_STACK_SIZE + 256U,
                    NULL, tskIDLE_PRIORITY + 2U, NULL) != pdPASS) {
        return 1;
    }
    if (xTaskCreate(applicationTask, "Application", configMINIMAL_STACK_SIZE + 256U,
                    NULL, tskIDLE_PRIORITY + 1U, &applicationTaskHandle) != pdPASS) {
        return 1;
    }

    puts("Starting FreeRTOS UART DMA round-trip demo...");
    vTaskStartScheduler();

    return (completedRoundTrips == DEMO_ROUND_TRIPS) ? 0 : 1;
}
