/** @file
 * @brief Native assertions for the HCI parser and UART DMA integration. */
#include "uart_dma/uart_dma.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static UART_TypeDef uartRegisters;
static DMA_Stream_TypeDef rxDmaRegisters;
static DMA_Stream_TypeDef txDmaRegisters;

/* These symbols satisfy the platform hooks declared by the library. */
UART_TypeDef *USART_HW = &uartRegisters;
DMA_Stream_TypeDef *DMA_RX_STREAM = &rxDmaRegisters;
DMA_Stream_TypeDef *DMA_TX_STREAM = &txDmaRegisters;

static unsigned int eventCount;
static unsigned int commandCount;
static unsigned int aclCount;
static uint8_t lastEventCode;
static uint8_t lastEventLength;
static uint16_t lastHandleFlags;
static uint16_t lastCommandOpcode;
static uint16_t lastAclLength;
static uint8_t capturedPayload[UART_DMA_MAX_PACKET_SIZE];

/** Capture an Event callback for subsequent assertions. */
static void captureEvent(uint8_t eventCode, uint8_t parameterLength,
                         const uint8_t *parameters) {
    eventCount++;
    lastEventCode = eventCode;
    lastEventLength = parameterLength;
    if (parameterLength != 0U) {
        memcpy(capturedPayload, parameters, parameterLength);
    }
}

/** Capture a Command callback for subsequent assertions. */
static void captureCommand(uint16_t opcode, uint8_t parameterLength,
                           const uint8_t *parameters) {
    commandCount++;
    lastCommandOpcode = opcode;
    lastEventLength = parameterLength;
    if (parameterLength != 0U) {
        memcpy(capturedPayload, parameters, parameterLength);
    }
}

/** Capture an ACL callback for subsequent assertions. */
static void captureAcl(uint16_t handleFlags, uint16_t dataLength,
                       const uint8_t *data) {
    aclCount++;
    lastHandleFlags = handleFlags;
    lastAclLength = dataLength;
    if (dataLength != 0U) {
        memcpy(capturedPayload, data, dataLength);
    }
}

/** Feed a contiguous test vector into the byte-oriented parser. */
static void feedBytes(const uint8_t *bytes, size_t length) {
    for (size_t index = 0; index < length; index++) {
        BleHci_ParseByte(bytes[index]);
    }
}

/** Verify fragmented and zero-length Event packets. */
static void testEventPackets(void) {
    static const uint8_t commandComplete[] = {
        0x04U, 0x0EU, 0x04U, 0x01U, 0x03U, 0x0CU, 0x00U
    };
    static const uint8_t zeroLengthEvent[] = {0x04U, 0x05U, 0x00U};

    BleHci_Reset();
    feedBytes(commandComplete, 2U);
    assert(eventCount == 0U);
    feedBytes(&commandComplete[2], sizeof(commandComplete) - 2U);
    assert(eventCount == 1U);
    assert(lastEventCode == 0x0EU);
    assert(lastEventLength == 4U);
    assert(memcmp(capturedPayload, &commandComplete[3], 4U) == 0);

    feedBytes(zeroLengthEvent, sizeof(zeroLengthEvent));
    assert(eventCount == 2U);
    assert(lastEventCode == 0x05U);
    assert(lastEventLength == 0U);
}

/** Verify HCI Command opcode and parameter decoding. */
static void testCommandPackets(void) {
    static const uint8_t reset[] = {0x01U, 0x03U, 0x0CU, 0x00U};
    static const uint8_t scanEnable[] = {
        0x01U, 0x0CU, 0x20U, 0x02U, 0x01U, 0x00U
    };

    BleHci_Reset();
    feedBytes(reset, sizeof(reset));
    assert(commandCount == 1U);
    assert(lastCommandOpcode == 0x0C03U);
    assert(lastEventLength == 0U);

    feedBytes(scanEnable, sizeof(scanEnable));
    assert(commandCount == 2U);
    assert(lastCommandOpcode == 0x200CU);
    assert(lastEventLength == 2U);
    assert(capturedPayload[0] == 0x01U);
    assert(capturedPayload[1] == 0x00U);
}

/** Verify maximum-sized ACL payload decoding. */
static void testAclPackets(void) {
    BleHciAclPacket_t maximumPacket = {
        .header = {
            .indicator = {.value = BLE_HCI_H4_ACL},
            .fields = {
                .handle_flags_le = BleHciWire_ToLe16(0x2042U),
                .data_length_le = BleHciWire_ToLe16(UART_DMA_MAX_PACKET_SIZE),
            },
        },
    };
    for (size_t index = 0; index < UART_DMA_MAX_PACKET_SIZE; index++) {
        maximumPacket.data[index] = (uint8_t)index;
    }

    BleHci_Reset();
    feedBytes((const uint8_t *)&maximumPacket,
              sizeof(maximumPacket.header) + UART_DMA_MAX_PACKET_SIZE);
    assert(aclCount == 1U);
    assert(lastHandleFlags == 0x2042U);
    assert(lastAclLength == UART_DMA_MAX_PACKET_SIZE);
    assert(memcmp(capturedPayload, maximumPacket.data, UART_DMA_MAX_PACKET_SIZE) == 0);
}

/** Verify that an unsupported indicator does not poison the next packet. */
static void testMalformedPacketRecovery(void) {
    static const uint8_t oversizedAclHeader[] = {
        0x02U, 0x01U, 0x00U, 0x01U, 0x01U
    };
    static const uint8_t validEvent[] = {0x04U, 0x3EU, 0x01U, 0x02U};
    const unsigned int eventsBefore = eventCount;
    const unsigned int aclBefore = aclCount;

    BleHci_Reset();
    BleHci_ParseByte(0xFFU);
    feedBytes(oversizedAclHeader, sizeof(oversizedAclHeader));
    feedBytes(validEvent, sizeof(validEvent));

    assert(aclCount == aclBefore);
    assert(eventCount == eventsBefore + 1U);
    assert(lastEventCode == 0x3EU);
    assert(lastEventLength == 1U);
    assert(capturedPayload[0] == 0x02U);
}

/** Verify queue argument checking and both ISR entry points. */
static void testUartApiAndInterrupts(void) {
    static const uint8_t arbitraryPayload[] = {0x01U, 0x02U, 0x03U, 0x04U};
    PacketMessage_t message = UartDma_MessageFrom(arbitraryPayload,
                                                  sizeof(arbitraryPayload));

    rxDmaRegisters.NDTR = UART_DMA_RX_BUFFER_SIZE;
    UartDmaSystem_Init();

    assert(UartDma_GetRxMode() == UART_DMA_RX_MODE_DMA_IDLE);
    assert((rxDmaRegisters.CR & 1UL) != 0U);
    assert((uartRegisters.CR1 & (1UL << 5)) == 0U);
    UartDma_SetRxMode(UART_DMA_RX_MODE_BYTE_IRQ);
    assert(UartDma_GetRxMode() == UART_DMA_RX_MODE_BYTE_IRQ);
    assert(UartDma_GetRxInterruptCount() == 0U);
    assert((rxDmaRegisters.CR & 1UL) == 0U);
    assert((uartRegisters.CR1 & (1UL << 5)) != 0U);

    static const uint8_t interruptBytes[] = {0xA5U, 0x5AU, 0x11U, 0xFEU};
    for (size_t index = 0U; index < sizeof(interruptBytes); index++) {
        uartRegisters.RDR = interruptBytes[index];
        uartRegisters.ISR = 1UL << 5;
        USART_IRQHandler();
    }
    uint8_t receivedBytes[sizeof(interruptBytes)] = {0U};
    uint32_t receivedLength = 0U;
    assert(UartDmaRingBuffer_ReadTo(UartDmaRingBuffer_GetWriteIndex(),
                                    receivedBytes, sizeof(receivedBytes),
                                    &receivedLength));
    assert(receivedLength == sizeof(interruptBytes));
    assert(memcmp(receivedBytes, interruptBytes, sizeof(interruptBytes)) == 0);
    assert(UartDma_GetRxInterruptCount() == sizeof(interruptBytes));

    UartDma_SetRxMode(UART_DMA_RX_MODE_DMA_IDLE);
    assert(UartDma_GetRxMode() == UART_DMA_RX_MODE_DMA_IDLE);
    assert((rxDmaRegisters.CR & 1UL) != 0U);
    assert((uartRegisters.CR1 & (1UL << 5)) == 0U);
    DMA_RX_Complete_IRQHandler();
    DMA_RX_Complete_IRQHandler();
    assert(UartDma_GetRxInterruptCount() == sizeof(interruptBytes) + 2U);

    assert(!UartDma_SetRxBufferSize(128U));
    assert(UartDma_SetRxBufferSize(4096U));
    assert(UartDma_GetRxBufferSize() == 4096U);
    assert(rxDmaRegisters.NDTR == 4096U);
    assert(UartDma_SetRxBufferSize(UART_DMA_RX_BUFFER_SIZE));

    assert(UartDma_SendPacket(NULL, 0U) == pdFALSE);
    message.length = UART_DMA_MAX_PACKET_SIZE - 1U;
    assert(UartDma_SendPacket(&message, 0U) == pdFALSE);
    message.length = UART_DMA_MAX_PACKET_SIZE - 2U;
    assert(UartDma_SendPacket(&message, 0U) == pdTRUE);

    uartRegisters.ISR = 1UL << 4;
    uartRegisters.ICR = 0U;
    USART_IRQHandler();
    assert(uartRegisters.ICR == (1UL << 4));

    uartRegisters.ISR = 0U;
    uartRegisters.ICR = 0U;
    USART_IRQHandler();
    assert(uartRegisters.ICR == 0U);
    DMA_TX_Complete_IRQHandler();
}

/** Install capture callbacks and execute all native assertions. */
int main(void) {
    BleHci_SetEventHandler(captureEvent);
    BleHci_SetCommandHandler(captureCommand);
    BleHci_SetAclHandler(captureAcl);

    testEventPackets();
    testCommandPackets();
    testAclPackets();
    testMalformedPacketRecovery();
    testUartApiAndInterrupts();

    BleHci_SetEventHandler(NULL);
    BleHci_SetCommandHandler(NULL);
    BleHci_SetAclHandler(NULL);
    puts("All UART DMA and BLE HCI tests passed.");
    return 0;
}
