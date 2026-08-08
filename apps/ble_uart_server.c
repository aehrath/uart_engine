/** @file
 * @brief Continuous FreeRTOS BLE-side server connected through a pseudo-UART. */
#include "uart_dma/uart_dma.h"
#include "pseudo_uart.h"
#include "serial_ingress.h"
#include "test_transfer_protocol.h"

#include "task.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define UART_IDLE_FLAG       (1UL << 4)
#define UART_RXNE_FLAG       (1UL << 5)
#define SERIAL_POLL_TICKS    pdMS_TO_TICKS(5U)
#define MAX_WIRE_FRAME       UART_DMA_MAX_PACKET_SIZE
#define TEST_TX_BATCH_SIZE   (64U * 1024U)

static UART_TypeDef uartRegisters;
static DMA_Stream_TypeDef rxDmaRegisters;
static DMA_Stream_TypeDef txDmaRegisters;

UART_TypeDef *USART_HW = &uartRegisters;
DMA_Stream_TypeDef *DMA_RX_STREAM = &rxDmaRegisters;
DMA_Stream_TypeDef *DMA_TX_STREAM = &txDmaRegisters;

static int ptyMaster = -1;
static uint32_t dmaWriteIndex;
static volatile sig_atomic_t stopRequested;
static volatile uint32_t outboundTestRequest;
static volatile bool outboundTestActive;
static uint32_t inboundTestIrqStart;
static bool inboundTestIrqActive;
static pthread_mutex_t ptyTxMutex = PTHREAD_MUTEX_INITIALIZER;

/** Running verification state for the chunked deterministic transfer protocol. */
typedef struct {
    /** Expected transfer identifier. */
    uint16_t identifier;
    /** Declared logical size. */
    uint32_t totalSize;
    /** Contiguous bytes verified so far. */
    uint32_t receivedSize;
    /** Next progress-report boundary. */
    uint32_t nextProgress;
    /** Whether a transfer is in progress. */
    bool active;
    /** Whether all metadata and generated data remain valid. */
    bool valid;
} TestTransferState_t;

static TestTransferState_t testTransfer;

/** Convert one hexadecimal digit to its numeric value. */
static int hexNibble(char value) {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

/** Decode dashboard-provided compact hexadecimal H4 bytes. */
static size_t parseDashboardPacket(const char *text, uint8_t *output,
                                   size_t capacity) {
    size_t length = 0U;
    while (*text != '\0') {
        while (*text == ' ' || *text == '\t' || *text == ':' || *text == ',') {
            text++;
        }
        if (*text == '\0') {
            break;
        }
        const int high = hexNibble(text[0]);
        const int low = hexNibble(text[1]);
        if (high < 0 || low < 0 || length == capacity) {
            return SIZE_MAX;
        }
        output[length++] = (uint8_t)((high << 4) | low);
        text += 2;
    }
    return length;
}

/** Record a termination request from a POSIX signal handler. */
static void requestStop(int signalNumber) {
    (void)signalNumber;
    stopRequested = 1;
}

/** Copy a complete serial frame into circular RX DMA and raise UART IDLE. */
static void injectDmaFrame(const uint8_t *frame, uint16_t length) {
    uint8_t *const dmaBuffer = UartDma_GetRxBuffer();
    const uint32_t dmaBufferSize = UartDma_GetRxBufferSize();

    if (UartDma_GetRxMode() == UART_DMA_RX_MODE_BYTE_IRQ) {
        for (uint16_t index = 0U; index < length; index++) {
            USART_HW->RDR = frame[index];
            USART_HW->ISR = UART_RXNE_FLAG;
            USART_IRQHandler();
            dmaWriteIndex = (dmaWriteIndex + 1U) % dmaBufferSize;
        }
    } else {
        for (uint16_t index = 0U; index < length; index++) {
            dmaBuffer[dmaWriteIndex] = frame[index];
            dmaWriteIndex = (dmaWriteIndex + 1U) % dmaBufferSize;
        }
    }

    DMA_RX_STREAM->NDTR = dmaBufferSize - dmaWriteIndex;
    USART_HW->ISR = UART_IDLE_FLAG;
    USART_IRQHandler();
    USART_HW->ISR = 0U;
}

/** Broadcast the authoritative receive mode as a typed vendor event. */
static void sendRxModeState(void) {
    const BleHciRxModeEvent_t event = {
        .header = {
            .indicator = {.value = BLE_HCI_H4_EVENT},
            .fields = {
                .event_code = BLE_HCI_VENDOR_EVENT_CODE,
                .parameter_length = sizeof(BleHciRxModeEventParameters_t),
            },
        },
        .parameters = {
            .signature = {'R', 'X', 'M', 'D'},
            .mode = UartDma_GetRxMode() == UART_DMA_RX_MODE_BYTE_IRQ
                ? BLE_HCI_RX_MODE_BYTE_IRQ : BLE_HCI_RX_MODE_DMA,
        },
    };
    PacketMessage_t response = UartDma_MessageFrom(&event, sizeof(event));
    (void)UartDma_SendPacket(&response, pdMS_TO_TICKS(100U));
}

/** Change the receiver at a frame boundary, log it, and notify the client. */
static void setRxMode(UartDmaRxMode_t mode, bool notifyClient) {
    UartDma_SetRxMode(mode);
    printf("RX MODE -> %s\n", mode == UART_DMA_RX_MODE_BYTE_IRQ ? "BYTE IRQ" : "DMA");
    fflush(stdout);
    if (notifyClient) {
        sendRxModeState();
    }
}

/** Log this server process's completed-transmission IRQ statistics once. */
static void reportRxIrqStats(uint32_t transmissionCount) {
    const uint32_t absoluteCount = UartDma_GetRxInterruptCount();
    printf("RX IRQ TRANSMISSION -> absolute=%u transmission=%u\n",
           (unsigned int)absoluteCount, (unsigned int)transmissionCount);
    fflush(stdout);
}

/** Recognize one chunk of the deterministic ACL transfer protocol. */
static bool testTransferFrameFlags(const uint8_t *frame, uint16_t length,
                                   uint8_t *flags) {
    if (length < sizeof(BleHciAclHeader_t) + sizeof(TestTransferHeader_t) +
                 sizeof(BleHciCrcTrailer_t)) {
        return false;
    }
    BleHciAclHeader_t acl;
    TestTransferHeader_t transfer;
    memcpy(&acl, frame, sizeof(acl));
    memcpy(&transfer, &frame[sizeof(acl)], sizeof(transfer));
    if (acl.indicator.value != BLE_HCI_H4_ACL ||
        (BleHciWire_FromLe16(acl.fields.handle_flags_le) & 0x0FFFU) !=
            TEST_TRANSFER_HANDLE ||
        transfer.version != 1U ||
        memcmp(transfer.signature, "TEST", sizeof(transfer.signature)) != 0) {
        return false;
    }
    *flags = transfer.flags;
    return true;
}

/** Queue one dashboard-originated H4 packet for the server TX engine. */
static void sendDashboardPacket(const char *encodedPacket) {
    PacketMessage_t packet = {{0U}, 0U};
    const size_t length = parseDashboardPacket(
        encodedPacket, packet.payload, sizeof(packet.payload) - sizeof(BleHciCrcTrailer_t));
    if (length == SIZE_MAX || length == 0U) {
        puts("ERROR  Dashboard supplied invalid H4 hexadecimal bytes");
        return;
    }
    packet.length = (uint32_t)length;
    if (UartDma_SendPacket(&packet, pdMS_TO_TICKS(1000U)) == pdTRUE) {
        printf("TX DMA <- dashboard H4 packet (%u bytes)\n", (unsigned int)length);
    } else {
        puts("ERROR  Dashboard H4 packet could not be queued");
    }
    fflush(stdout);
}

/** Adapter from serial-ingress callbacks to simulated DMA injection. */
static void handleSerialFrame(const uint8_t *frame, uint16_t length, void *context) {
    (void)context;
    const uint32_t before = UartDma_GetRxInterruptCount();
    uint8_t testFlags = 0U;
    const bool isTestTransfer = testTransferFrameFlags(frame, length, &testFlags);
    if (isTestTransfer && (testFlags & TEST_TRANSFER_FLAG_START) != 0U) {
        inboundTestIrqStart = before;
        inboundTestIrqActive = true;
    }
    injectDmaFrame(frame, length);
    const uint32_t after = UartDma_GetRxInterruptCount();
    if (isTestTransfer && inboundTestIrqActive) {
        if ((testFlags & TEST_TRANSFER_FLAG_END) != 0U) {
            reportRxIrqStats(after - inboundTestIrqStart);
            inboundTestIrqActive = false;
        }
    } else {
        reportRxIrqStats(after - before);
    }
}

/** Write an entire byte span while the caller owns the pseudo-UART TX lock. */
static bool writeAllUnlocked(const uint8_t *data, size_t size) {
    size_t written = 0U;
    while (written < size && !stopRequested) {
        const ssize_t result = write(ptyMaster, &data[written], size - written);
        if (result > 0) {
            written += (size_t)result;
        } else if (errno == EAGAIN || errno == EINTR) {
            struct pollfd output = {ptyMaster, POLLOUT, 0};
            (void)poll(&output, 1U, 10);
        } else {
            return false;
        }
    }
    return written == size;
}

/** Write one complete span without interleaving another server transmission. */
static bool writeAll(const uint8_t *data, size_t size) {
    (void)pthread_mutex_lock(&ptyTxMutex);
    const bool written = writeAllUnlocked(data, size);
    (void)pthread_mutex_unlock(&ptyTxMutex);
    return written;
}

/** Host implementation of the platform TX DMA hook. */
void UartDma_PlatformTransmitDma(uint8_t *data, uint16_t size) {
    (void)writeAll(data, size);
    DMA_TX_Complete_IRQHandler();
}

/** Log an HCI command and enqueue its typed Command Complete response. */
static void onHciCommand(uint16_t opcode, uint8_t parameterLength,
                         const uint8_t *parameters) {
    if (opcode == BLE_HCI_RX_MODE_OPCODE &&
        parameterLength == sizeof(BleHciRxModeCommandParameters_t)) {
        BleHciRxModeCommandParameters_t request;
        memcpy(&request, parameters, sizeof(request));
        if (request.mode == BLE_HCI_RX_MODE_DMA) {
            setRxMode(UART_DMA_RX_MODE_DMA_IDLE, true);
        } else if (request.mode == BLE_HCI_RX_MODE_BYTE_IRQ) {
            setRxMode(UART_DMA_RX_MODE_BYTE_IRQ, true);
        } else {
            sendRxModeState();
        }
        return;
    }
    const BleHciCommandCompleteEvent_t event = {
        .header = {
            .indicator = {.value = BLE_HCI_H4_EVENT},
            .fields = {
                .event_code = 0x0EU,
                .parameter_length = 4U,
            },
        },
        .parameters = {
            .command_packets = 1U,
            .opcode_le = BleHciWire_ToLe16(opcode),
            .status = 0U,
        },
    };
    PacketMessage_t response = UartDma_MessageFrom(&event, sizeof(event));

    printf("RX DMA -> HCI command 0x%04X (%u parameter byte%s)\n",
           opcode, parameterLength, parameterLength == 1U ? "" : "s");
    if (parameterLength != 0U) {
        printf("RX DATA: ");
        for (uint8_t index = 0U; index < parameterLength; index++) {
            printf("%02X%s", parameters[index],
                   index + 1U == parameterLength ? "\n" : " ");
        }
    }
    printf("TX DMA <- Command Complete 0x%04X\n", opcode);
    fflush(stdout);
    (void)UartDma_SendPacket(&response, pdMS_TO_TICKS(100U));
}

/** Read dashboard/terminal control lines without blocking the scheduler. */
static void controlReaderTask(void *parameters) {
    (void)parameters;
    char command[2048];
    size_t length = 0U;
    (void)fcntl(STDIN_FILENO, F_SETFL, fcntl(STDIN_FILENO, F_GETFL) | O_NONBLOCK);
    while (!stopRequested) {
        char byte;
        const ssize_t count = read(STDIN_FILENO, &byte, sizeof(byte));
        if (count == 1) {
            if (byte == '\n') {
                command[length] = '\0';
                if (strcmp(command, "irq") == 0) {
                    setRxMode(UART_DMA_RX_MODE_BYTE_IRQ, true);
                } else if (strcmp(command, "dma") == 0) {
                    setRxMode(UART_DMA_RX_MODE_DMA_IDLE, true);
                } else if (strcmp(command, "mode") == 0) {
                    sendRxModeState();
                } else if (strcmp(command, "stats") == 0) {
                    printf("RX IRQ COUNT -> %u\n",
                           (unsigned int)UartDma_GetRxInterruptCount());
                    fflush(stdout);
                } else if (strncmp(command, "tx ", 3U) == 0) {
                    sendDashboardPacket(&command[3]);
                } else if (strncmp(command, "test ", 5U) == 0) {
                    char *end = NULL;
                    const unsigned long requested = strtoul(&command[5], &end, 10);
                    if (*end != '\0' || requested == 0UL || requested > UINT32_MAX) {
                        puts("ERROR  Dashboard supplied invalid test-transfer size");
                    } else if (outboundTestActive || outboundTestRequest != 0U) {
                        puts("ERROR  A server test transfer is already active");
                    } else {
                        outboundTestRequest = (uint32_t)requested;
                    }
                    fflush(stdout);
                }
                length = 0U;
            } else if (byte != '\r' && length + 1U < sizeof(command)) {
                command[length++] = byte;
            }
        } else {
            vTaskDelay(SERIAL_POLL_TICKS);
        }
    }
    vTaskDelete(NULL);
}

/** Generate dashboard-requested deterministic ACL traffic through TX DMA. */
static void outboundTestTask(void *parameters) {
    (void)parameters;
    static TestTransferPacket_t acl;
    static uint8_t wireBatch[TEST_TX_BATCH_SIZE];
    uint16_t identifier = 0U;
    while (!stopRequested) {
        const uint32_t totalSize = outboundTestRequest;
        if (totalSize == 0U) {
            vTaskDelay(SERIAL_POLL_TICKS);
            continue;
        }
        outboundTestRequest = 0U;
        outboundTestActive = true;
        identifier++;
        if (identifier == 0U) {
            identifier = 1U;
        }
        printf("TX DMA <- TEST message start id=%u size=%u bytes\n",
               identifier, (unsigned int)totalSize);
        fflush(stdout);

        uint32_t offset = 0U;
        uint32_t nextProgress = TEST_TRANSFER_PROGRESS_STEP;
        size_t batchLength = 0U;
        bool sent = true;
        while (offset < totalSize && !stopRequested) {
            const uint32_t remaining = totalSize - offset;
            const uint16_t chunkSize = (uint16_t)(remaining < TEST_TRANSFER_CHUNK_DATA_SIZE
                ? remaining : TEST_TRANSFER_CHUNK_DATA_SIZE);
            const uint16_t aclLength = (uint16_t)(TEST_TRANSFER_HEADER_SIZE + chunkSize);
            acl = (TestTransferPacket_t){
                .h4 = {
                    .indicator = {.value = BLE_HCI_H4_ACL},
                    .fields = {
                        .handle_flags_le = BleHciWire_ToLe16(TEST_TRANSFER_HANDLE),
                        .data_length_le = BleHciWire_ToLe16(aclLength),
                    },
                },
                .transfer = {
                    .signature = {'T', 'E', 'S', 'T'},
                    .version = 1U,
                    .flags = (offset == 0U ? TEST_TRANSFER_FLAG_START : 0U)
                           | (offset + chunkSize == totalSize ? TEST_TRANSFER_FLAG_END : 0U),
                    .identifier_le = BleHciWire_ToLe16(identifier),
                    .total_size_le = BleHciWire_ToLe32(totalSize),
                    .offset_le = BleHciWire_ToLe32(offset),
                    .chunk_size_le = BleHciWire_ToLe16(chunkSize),
                },
            };
            for (uint16_t index = 0U; index < chunkSize; index++) {
                acl.data[index] = TestTransfer_PatternByte(offset + index);
            }

            const size_t h4Length = sizeof(acl.h4) + aclLength;
            const BleHciCrcTrailer_t trailer = {
                BleHciWire_ToBe16(UartDma_ComputeCrc16((const uint8_t *)&acl, h4Length))
            };
            const size_t wireLength = h4Length + sizeof(trailer);
            if (batchLength + wireLength > sizeof(wireBatch)) {
                if (!writeAll(wireBatch, batchLength)) {
                    sent = false;
                    break;
                }
                batchLength = 0U;
            }
            memcpy(&wireBatch[batchLength], &acl, h4Length);
            batchLength += h4Length;
            memcpy(&wireBatch[batchLength], &trailer, sizeof(trailer));
            batchLength += sizeof(trailer);

            if (batchLength >= sizeof(wireBatch) - MAX_WIRE_FRAME &&
                !writeAll(wireBatch, batchLength)) {
                sent = false;
                break;
            }
            if (batchLength >= sizeof(wireBatch) - MAX_WIRE_FRAME) {
                batchLength = 0U;
            }
            offset += chunkSize;
            if (offset >= nextProgress) {
                printf("TX DMA <- TEST progress %u/%u bytes (%u%%)\n",
                       (unsigned int)offset, (unsigned int)totalSize,
                       (unsigned int)((offset * 100ULL) / totalSize));
                fflush(stdout);
                nextProgress += TEST_TRANSFER_PROGRESS_STEP;
            }
        }
        if (sent && batchLength != 0U && !writeAll(wireBatch, batchLength)) {
            sent = false;
        }
        printf("TX DMA <- TEST message %s id=%u size=%u bytes\n",
               sent && offset == totalSize ? "complete" : "failed",
               identifier, (unsigned int)offset);
        fflush(stdout);
        outboundTestActive = false;
    }
    vTaskDelete(NULL);
}

/** Enqueue the vendor event reporting test-transfer verification status. */
static void sendTestTransferComplete(uint16_t identifier, uint32_t totalSize,
                                     bool valid) {
    const BleHciTestCompleteEvent_t event = {
        .header = {
            .indicator = {.value = BLE_HCI_H4_EVENT},
            .fields = {
                .event_code = 0xFFU,
                .parameter_length = 0x0BU,
            },
        },
        .parameters = {
            .signature = {'T', 'E', 'S', 'T'},
            .identifier_le = BleHciWire_ToLe16(identifier),
            .status = valid ? 0U : 1U,
            .total_size_le = BleHciWire_ToLe32(totalSize),
        },
    };
    PacketMessage_t response = UartDma_MessageFrom(&event, sizeof(event));
    (void)UartDma_SendPacket(&response, pdMS_TO_TICKS(100U));
}

/** Print every byte of a small test message in a readable hex dump. */
static void printSmallTestMessage(const uint8_t *data, uint32_t length) {
    for (uint32_t offset = 0U; offset < length; offset += 16U) {
        const uint32_t lineLength = (length - offset) < 16U ? (length - offset) : 16U;
        printf("RX DATA %04X: ", (unsigned int)offset);
        for (uint32_t index = 0U; index < lineLength; index++) {
            printf("%02X%s", data[offset + index],
                   index + 1U == lineLength ? "\n" : " ");
        }
    }
}

/** Validate ordinary ACL traffic or consume one deterministic test chunk. */
static void onHciAcl(uint16_t handleFlags, uint16_t dataLength, const uint8_t *data) {
    TestTransferHeader_t header = {0};
    if (dataLength >= sizeof(header)) {
        memcpy(&header, data, sizeof(header));
    }
    if ((handleFlags & 0x0FFFU) != TEST_TRANSFER_HANDLE ||
        dataLength < sizeof(header) ||
        memcmp(header.signature, "TEST", sizeof(header.signature)) != 0 ||
        header.version != 1U) {
        printf("RX DMA -> ACL handle=0x%04X length=%u\n", handleFlags, dataLength);
        const uint16_t preview = dataLength < 32U ? dataLength : 32U;
        if (preview != 0U) {
            printf("RX DATA: ");
            for (uint16_t index = 0U; index < preview; index++) {
                printf("%02X%s", data[index], index + 1U == preview ? "\n" : " ");
            }
        }
        fflush(stdout);
        return;
    }

    const uint8_t flags = header.flags;
    const uint16_t identifier = TestTransfer_Identifier(&header);
    const uint32_t totalSize = TestTransfer_TotalSize(&header);
    const uint32_t offset = TestTransfer_Offset(&header);
    const uint16_t chunkSize = TestTransfer_ChunkSize(&header);
    if ((uint32_t)chunkSize + TEST_TRANSFER_HEADER_SIZE != dataLength) {
        puts("RX DMA -> TEST rejected: invalid chunk length");
        fflush(stdout);
        return;
    }

    if ((flags & TEST_TRANSFER_FLAG_START) != 0U) {
        testTransfer.identifier = identifier;
        testTransfer.totalSize = totalSize;
        testTransfer.receivedSize = 0U;
        testTransfer.nextProgress = TEST_TRANSFER_PROGRESS_STEP;
        testTransfer.active = true;
        testTransfer.valid = true;
        printf("RX DMA -> TEST message start id=%u size=%u bytes\n",
               identifier, (unsigned int)totalSize);
    }

    if (!testTransfer.active || identifier != testTransfer.identifier ||
        totalSize != testTransfer.totalSize || offset != testTransfer.receivedSize) {
        testTransfer.valid = false;
    }
    for (uint16_t index = 0U; index < chunkSize; index++) {
        if (data[TEST_TRANSFER_HEADER_SIZE + index] !=
            TestTransfer_PatternByte(offset + index)) {
            testTransfer.valid = false;
            break;
        }
    }
    testTransfer.receivedSize += chunkSize;

    if (totalSize <= 128U) {
        printSmallTestMessage(&data[TEST_TRANSFER_HEADER_SIZE], chunkSize);
    } else if (testTransfer.receivedSize >= testTransfer.nextProgress) {
        printf("RX DMA -> TEST progress %u/%u bytes (%u%%)\n",
               (unsigned int)testTransfer.receivedSize, (unsigned int)totalSize,
               (unsigned int)((testTransfer.receivedSize * 100ULL) / totalSize));
        testTransfer.nextProgress += TEST_TRANSFER_PROGRESS_STEP;
    }

    if ((flags & TEST_TRANSFER_FLAG_END) != 0U) {
        const bool valid = testTransfer.valid && testTransfer.receivedSize == totalSize;
        printf("RX DMA -> TEST message complete id=%u size=%u verified=%s\n",
               identifier, (unsigned int)testTransfer.receivedSize, valid ? "yes" : "NO");
        printf("TX DMA <- TEST completion id=%u status=%s\n",
               identifier, valid ? "success" : "failed");
        fflush(stdout);
        sendTestTransferComplete(identifier, totalSize, valid);
        testTransfer.active = false;
    }
}

/** FreeRTOS task that reads the pseudo-terminal and reassembles H4 frames. */
static void serialReaderTask(void *parameters) {
    (void)parameters;
    static ServerSerialIngress_t ingress;
    ServerSerialIngress_Init(&ingress);
    static uint8_t input[16U * 1024U];

    while (!stopRequested) {
        const ssize_t count = read(ptyMaster, input, sizeof(input));
        if (count > 0) {
            ServerSerialIngress_Feed(&ingress, input, (size_t)count,
                                     handleSerialFrame, NULL);
        } else {
            vTaskDelay(SERIAL_POLL_TICKS);
        }
    }

    vTaskEndScheduler();
}

/** Create the pseudo-UART and run the continuous FreeRTOS scheduler. */
int main(void) {
    char slaveName[128];
    ptyMaster = PseudoUart_Open(slaveName, sizeof(slaveName));
    if (ptyMaster < 0) {
        perror("Unable to create pseudo UART");
        return 1;
    }

    (void)signal(SIGINT, requestStop);
    (void)signal(SIGTERM, requestStop);
    setvbuf(stdout, NULL, _IOLBF, 0U);

    DMA_RX_STREAM->NDTR = UartDma_GetRxBufferSize();
    BleHci_SetCommandHandler(onHciCommand);
    BleHci_SetAclHandler(onHciAcl);
    UartDmaSystem_Init();
    setRxMode(UART_DMA_RX_MODE_DMA_IDLE, false);

    if (xTaskCreate(serialReaderTask, "Serial_RX", configMINIMAL_STACK_SIZE + 256U,
                    NULL, tskIDLE_PRIORITY + 1U, NULL) != pdPASS) {
        PseudoUart_Close(ptyMaster);
        return 1;
    }
    if (xTaskCreate(controlReaderTask, "Mode_Control", configMINIMAL_STACK_SIZE + 128U,
                    NULL, tskIDLE_PRIORITY + 1U, NULL) != pdPASS) {
        PseudoUart_Close(ptyMaster);
        return 1;
    }
    if (xTaskCreate(outboundTestTask, "Dashboard_TX", configMINIMAL_STACK_SIZE + 512U,
                    NULL, tskIDLE_PRIORITY + 1U, NULL) != pdPASS) {
        PseudoUart_Close(ptyMaster);
        return 1;
    }

    puts("Continuous BLE C UART server is running.");
    printf("In the Python app, type this port manually: %s\n", slaveName);
    puts("Baud rate is ignored for the pseudo-UART. Press Ctrl-C to stop.");
    vTaskStartScheduler();

    PseudoUart_Close(ptyMaster);
    puts("BLE C UART server stopped.");
    return 0;
}
