/** @file
 * @brief Portable raylib/raygui BLE HCI UART client application. */
#include "raylib.h"
#include "client_preferences.h"
#include "dashboard_config.h"
#include "dashboard_dropdown.h"
#include "dashboard_log.h"
#include "dashboard_pipeline.h"
#include "dashboard_ui.h"
#include "h4_wire.h"
#include "lpuart_sim.h"
#include "serial_port.h"
#include "test_transfer_protocol.h"
#include "uart_dma/app_version.h"

#define RAYGUI_IMPLEMENTATION
#include "raygui.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_LOG_LENGTH      APP_DASHBOARD_LOG_LINE_SIZE
#define CONNECTION_PROBE_SIZE 128U
#define CONNECTION_PROBE_TIMEOUT_SECONDS 2.5


/** Fixed-height rolling traffic log displayed by the UI. */
typedef struct {
    /** Retained scrollable traffic messages. */
    AppDashboardLog_t messages;
    /** Complete H4 packets transmitted by this client. */
    uint32_t transmittedPackets;
    /** Complete H4 packets received by this client. */
    uint32_t receivedPackets;
    /** Authoritative receive mode reported by the connected server. */
    bool byteIrqMode;
    /** Receive mode currently active while a toggle request is in flight. */
    bool receiveByteIrqActive;
    /** App-local absolute count of per-byte receive-handler calls. */
    uint32_t absoluteIrqCount;
    /** Receive-handler calls used by the most recently completed transmission. */
    uint32_t transmissionIrqCount;
    /** Per-byte receive calls accumulated across a chunked test transmission. */
    uint32_t testIrqCalls;
    /** Whether an inbound test transmission is currently being accumulated. */
    bool testIrqActive;
    /** Next logical byte offset that produces a compact receive-progress log. */
    uint32_t testLogNextProgress;
} TrafficLog_t;

/** Incremental state for a nonblocking deterministic test transfer. */
typedef struct {
    /** Current transfer identifier. */
    uint16_t identifier;
    /** Requested logical byte count. */
    uint32_t totalSize;
    /** Next logical byte offset to send. */
    uint32_t offset;
    /** Offset at which another progress line is emitted. */
    uint32_t nextProgress;
    /** Whether the render loop should continue producing chunks. */
    bool active;
    /** Monotonic start used to pace both directions from selected baud. */
    double startedAt;
} TestSender_t;

/** State of the connection-time server identity check. */
typedef struct {
    /** Identifier assigned to the current or most recent probe. */
    uint16_t identifier;
    /** Monotonic time at which the current probe expires. */
    double deadline;
    /** Whether a matching acknowledgement is still expected. */
    bool pending;
    /** Whether the selected UART has been verified as this app's server. */
    bool verified;
    /** Whether an invalid acknowledgement requires an immediate disconnect. */
    bool failed;
    /** Whether successful verification has been announced to the UI. */
    bool readyAnnounced;
} ConnectionHandshake_t;

/** Objects used by the frame callback to update UI and handshake state. */
typedef struct {
    /** Rolling UI traffic log. */
    TrafficLog_t *log;
    /** Active connection verification state. */
    ConnectionHandshake_t *handshake;
    /** App-local producer/consumer telemetry updated by completed frames. */
    AppDashboardPipeline_t *pipeline;
    /** H4 bytes validated by the producer during the current sample. */
    uint64_t *producedSinceSample;
    /** H4 bytes synchronously delivered to the consumer during the sample. */
    uint64_t *consumedSinceSample;
} ClientFrameContext_t;

/** Append a formatted line, evicting the oldest line when full. */
static void appendLog(TrafficLog_t *log, const char *format, ...) {
    char line[MAX_LOG_LENGTH];
    va_list arguments;
    va_start(arguments, format);
    (void)vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);
    AppDashboardLog_Append(&log->messages, "%s", line);
}

/** Select the established traffic color for one retained client log line. */
static Color clientLogColor(const char *line) {
    return strncmp(line, "ERROR", 5U) == 0 ? (Color){255, 120, 120, 255}
        : strncmp(line, "TX", 2U) == 0 ? (Color){100, 190, 255, 255}
        : strncmp(line, "RX", 2U) == 0 ? (Color){105, 225, 150, 255}
        : (Color){190, 198, 210, 255};
}

/** Send one typed packet and CRC trailer, optionally recording its bytes. */
static bool sendFramedEx(AppSerialPort_t *port, TrafficLog_t *log,
                         const void *h4Packet, size_t h4Length,
                         bool logPacket) {
    if (!port->connected) {
        appendLog(log, "ERROR  Connect to a serial port first");
        return false;
    }
    if (h4Packet == NULL || h4Length > BLE_HCI_MAX_WIRE_PACKET) {
        appendLog(log, "ERROR  Packet is too large");
        return false;
    }
    const uint16_t crc = AppH4_Crc16(h4Packet, h4Length);
    const BleHciCrcTrailer_t trailer = {BleHciWire_ToBe16(crc)};
    uint8_t wirePacket[BLE_HCI_MAX_WIRE_PACKET + sizeof(trailer)];
    memcpy(wirePacket, h4Packet, h4Length);
    memcpy(&wirePacket[h4Length], &trailer, sizeof(trailer));
    char hex[MAX_LOG_LENGTH - 24U];
    AppH4_FormatHex(hex, sizeof(hex), h4Packet, h4Length);
    if (!AppSerialPort_Write(port, wirePacket, h4Length + sizeof(trailer))) {
        appendLog(log, "ERROR  Serial write failed");
        return false;
    }
    log->transmittedPackets++;
    if (logPacket) {
        appendLog(log, "TX  %s  [CRC %04X]", hex, crc);
    }
    return true;
}

/** Send and log one typed H4 packet. */
static bool sendFramed(AppSerialPort_t *port, TrafficLog_t *log,
                       const void *h4Packet, size_t h4Length) {
    return sendFramedEx(port, log, h4Packet, h4Length, true);
}

/** Construct and send a typed HCI Command packet. */
static bool sendCommand(AppSerialPort_t *port, TrafficLog_t *log,
                        uint16_t opcode, const uint8_t *parameters,
                        size_t parameterLength) {
    BleHciCommandPacket_t command;
    const size_t commandLength = AppH4Command_Init(
        &command, opcode, parameters, parameterLength);
    if (commandLength == 0U) {
        appendLog(log, "ERROR  HCI command parameters exceed 255 bytes");
        return false;
    }
    return sendFramed(port, log, &command, commandLength);
}

/** Send the single deterministic transfer used to identify the server UART. */
static bool sendConnectionProbe(AppSerialPort_t *port, TrafficLog_t *log,
                                ConnectionHandshake_t *handshake) {
    handshake->identifier++;
    if (handshake->identifier == 0U) {
        handshake->identifier = 1U;
    }
    TestTransferPacket_t probe = {
        .h4 = {
            .indicator = {.value = BLE_HCI_H4_ACL},
            .fields = {
                .handle_flags_le = BleHciWire_ToLe16(TEST_TRANSFER_HANDLE),
                .data_length_le = BleHciWire_ToLe16(
                    TEST_TRANSFER_HEADER_SIZE + CONNECTION_PROBE_SIZE),
            },
        },
        .transfer = {
            .signature = {'T', 'E', 'S', 'T'},
            .version = 1U,
            .flags = TEST_TRANSFER_FLAG_START | TEST_TRANSFER_FLAG_END,
            .identifier_le = BleHciWire_ToLe16(handshake->identifier),
            .total_size_le = BleHciWire_ToLe32(CONNECTION_PROBE_SIZE),
            .offset_le = BleHciWire_ToLe32(0U),
            .chunk_size_le = BleHciWire_ToLe16(CONNECTION_PROBE_SIZE),
        },
    };
    for (uint32_t index = 0U; index < CONNECTION_PROBE_SIZE; index++) {
        probe.data[index] = TestTransfer_PatternByte(index);
    }
    const size_t packetLength = sizeof(probe.h4) +
        TEST_TRANSFER_HEADER_SIZE + CONNECTION_PROBE_SIZE;
    if (!sendFramedEx(port, log, &probe, packetLength, false)) {
        return false;
    }
    handshake->pending = true;
    handshake->verified = false;
    handshake->failed = false;
    handshake->readyAnnounced = false;
    handshake->deadline = GetTime() + CONNECTION_PROBE_TIMEOUT_SECONDS;
    appendLog(log, "VERIFY  Sent 128-byte server probe (id=%u)",
              handshake->identifier);
    return true;
}

/** Convert a received frame callback into a traffic-log entry. */
static void logFrame(const uint8_t *frame, size_t length, bool crcValid,
                     void *context) {
    ClientFrameContext_t *const frameContext = context;
    TrafficLog_t *const log = frameContext->log;
    ConnectionHandshake_t *const handshake = frameContext->handshake;
    AppDashboardPipeline_t *const pipeline = frameContext->pipeline;
    const uint32_t wireLength = (uint32_t)length;
    const uint32_t irqCalls = log->receiveByteIrqActive ? wireLength
        : pipeline->dmaCapacity == 0U ? 0U
        : (wireLength + pipeline->dmaCapacity - 1U) / pipeline->dmaCapacity;
    log->absoluteIrqCount += irqCalls;
    pipeline->ringBurstBytes = wireLength < pipeline->ringCapacity
        ? wireLength : pipeline->ringCapacity;
    if (pipeline->ringBurstBytes > pipeline->ringHighWaterBytes) {
        pipeline->ringHighWaterBytes = pipeline->ringBurstBytes;
    }
    pipeline->dmaBurstBytes = wireLength < pipeline->dmaCapacity
        ? wireLength : pipeline->dmaCapacity;
    if (pipeline->dmaBurstBytes > pipeline->dmaHighWaterBytes) {
        pipeline->dmaHighWaterBytes = pipeline->dmaBurstBytes;
    }
    if (wireLength > pipeline->ringCapacity) {
        pipeline->ringDrops++;
        appendLog(log, "ERROR  RX ring overwrite: frame=%u capacity=%u",
                  (unsigned int)wireLength,
                  (unsigned int)pipeline->ringCapacity);
        return;
    }
    if (!crcValid) frameContext->pipeline->crcDrops++;
    if (crcValid) {
        const uint64_t h4Length = wireLength - sizeof(BleHciCrcTrailer_t);
        *frameContext->producedSinceSample += h4Length;
        *frameContext->consumedSinceSample += h4Length;
        /* Mirror the server's FreeRTOS producer/consumer hand-off. The
         * synchronous UI consumer drains immediately, but the enqueue still
         * reaches a one-packet high-water mark in the shared 16-slot queue. */
        if (pipeline->queueHighWater < 1U) {
            pipeline->queueHighWater = 1U;
        }
        pipeline->queueDepth = 1U;
    }
    log->receivedPackets++;
    const size_t h4Length = length - 2U;
    BleHciPacketIndicator_t indicator;
    memcpy(&indicator, frame, sizeof(indicator));
    uint16_t completionIdentifier = 0U;
    uint8_t completionStatus = 0U;
    uint32_t completionSize = 0U;
    const bool testCompletion = crcValid && AppH4TestCompleteEvent_Decode(
        frame, length, &completionIdentifier, &completionStatus, &completionSize);
    if (handshake->pending && testCompletion &&
        completionIdentifier == handshake->identifier) {
        handshake->pending = false;
        if (completionStatus == 0U && completionSize == CONNECTION_PROBE_SIZE) {
            handshake->verified = true;
            appendLog(log, "VERIFY  BLE UART server acknowledged probe");
        } else {
            handshake->failed = true;
        }
    }
    bool testFrame = false;
    uint8_t testFlags = 0U;
    uint32_t testTotal = 0U;
    uint32_t testOffset = 0U;
    uint16_t testChunk = 0U;
    if (h4Length >= sizeof(BleHciAclHeader_t) + sizeof(TestTransferHeader_t)) {
        BleHciAclHeader_t acl;
        TestTransferHeader_t transfer;
        memcpy(&acl, frame, sizeof(acl));
        memcpy(&transfer, &frame[sizeof(acl)], sizeof(transfer));
        testFrame = acl.indicator.value == BLE_HCI_H4_ACL &&
            (BleHciWire_FromLe16(acl.fields.handle_flags_le) & 0x0FFFU) ==
                TEST_TRANSFER_HANDLE &&
            transfer.version == 1U &&
            memcmp(transfer.signature, "TEST", sizeof(transfer.signature)) == 0;
        testFlags = transfer.flags;
        testTotal = TestTransfer_TotalSize(&transfer);
        testOffset = TestTransfer_Offset(&transfer);
        testChunk = TestTransfer_ChunkSize(&transfer);
    }
    bool irqCompletion = !testFrame;
    if (testFrame && (testFlags & TEST_TRANSFER_FLAG_START) != 0U) {
        log->testIrqCalls = 0U;
        log->testIrqActive = true;
        log->testLogNextProgress = TEST_TRANSFER_PROGRESS_STEP;
    }
    if (testFrame && log->testIrqActive) {
        log->testIrqCalls += irqCalls;
    }
    if (testFrame && log->testIrqActive &&
        (testFlags & TEST_TRANSFER_FLAG_END) != 0U) {
        log->transmissionIrqCount = log->testIrqCalls;
        log->testIrqActive = false;
        irqCompletion = true;
    } else if (!testFrame) {
        log->transmissionIrqCount = irqCalls;
    }
    const bool suppressBytes = testFrame && testTotal > 128U;
    if (suppressBytes) {
        const uint32_t received = testOffset + testChunk;
        if ((testFlags & TEST_TRANSFER_FLAG_START) != 0U) {
            appendLog(log, "RX  TEST start: %u bytes", (unsigned int)testTotal);
        }
        if ((testFlags & TEST_TRANSFER_FLAG_END) != 0U) {
            appendLog(log, "RX  TEST complete: %u/%u bytes",
                      (unsigned int)received, (unsigned int)testTotal);
        } else if (received >= log->testLogNextProgress) {
            appendLog(log, "RX  TEST progress: %u/%u bytes (%u%%)",
                      (unsigned int)received, (unsigned int)testTotal,
                      (unsigned int)((received * 100ULL) / testTotal));
            log->testLogNextProgress += TEST_TRANSFER_PROGRESS_STEP;
        }
    } else {
        char hex[MAX_LOG_LENGTH - 56U];
        AppH4_FormatHex(hex, sizeof(hex), frame, length);
        if (indicator.value == APP_H4_EVENT &&
            h4Length >= sizeof(BleHciEventHeader_t)) {
            BleHciEventHeader_t header;
            memcpy(&header, frame, sizeof(header));
            appendLog(log, "RX  %s  [%s, Event %02X]", hex,
                      crcValid ? "CRC OK" : "CRC BAD", header.fields.event_code);
        } else {
            appendLog(log, "RX  %s  [%s]", hex,
                      crcValid ? "CRC OK" : "CRC BAD");
        }
    }
    if (irqCompletion) {
        appendLog(log, "RX  IRQ complete: %u calls; app total=%u",
                  (unsigned int)log->transmissionIrqCount,
                  (unsigned int)log->absoluteIrqCount);
    }
}


/** Initialize the UI's incremental test-transfer state. */
static void startTestTransfer(TestSender_t *sender, AppSerialPort_t *port,
                              TrafficLog_t *log, uint32_t totalSize) {
    if (!port->connected) {
        appendLog(log, "ERROR  Connect before sending test data");
        return;
    }
    sender->identifier++;
    if (sender->identifier == 0U) {
        sender->identifier = 1U;
    }
    sender->totalSize = totalSize;
    sender->offset = 0U;
    sender->nextProgress = TEST_TRANSFER_PROGRESS_STEP;
    sender->active = true;
    sender->startedAt = GetTime();
    appendLog(log, "TEST  Sending %u bytes (id=%u)",
              (unsigned int)totalSize, sender->identifier);
}

/** Generate and send a bounded batch of test chunks during one render frame. */
static void pumpTestTransfer(TestSender_t *sender, AppSerialPort_t *port,
                             TrafficLog_t *log, uint32_t baud) {
    if (!sender->active) {
        return;
    }
    const uint64_t available = (uint64_t)((GetTime() - sender->startedAt) *
                               ((double)baud / 8.0));
    const uint32_t target = available < sender->totalSize
        ? (uint32_t)available : sender->totalSize;
    static uint8_t wireBatch[64U * 1024U];
    size_t batchLength = 0U;
    uint32_t batchPackets = 0U;
    while (sender->offset < target) {
        const uint32_t remaining = sender->totalSize - sender->offset;
        const uint16_t chunkSize = (uint16_t)(remaining < TEST_TRANSFER_CHUNK_DATA_SIZE
            ? remaining : TEST_TRANSFER_CHUNK_DATA_SIZE);
        TestTransferPacket_t acl;
        const uint16_t aclLength = (uint16_t)(TEST_TRANSFER_HEADER_SIZE + chunkSize);
        acl.h4 = (BleHciAclHeader_t){
            .indicator = {.value = BLE_HCI_H4_ACL},
            .fields = {
                .handle_flags_le = BleHciWire_ToLe16(TEST_TRANSFER_HANDLE),
                .data_length_le = BleHciWire_ToLe16(aclLength),
            },
        };
        acl.transfer = (TestTransferHeader_t){
            .signature = {'T', 'E', 'S', 'T'},
            .version = 1U,
            .flags = (sender->offset == 0U ? TEST_TRANSFER_FLAG_START : 0U)
                   | (sender->offset + chunkSize == sender->totalSize
                      ? TEST_TRANSFER_FLAG_END : 0U),
            .identifier_le = BleHciWire_ToLe16(sender->identifier),
            .total_size_le = BleHciWire_ToLe32(sender->totalSize),
            .offset_le = BleHciWire_ToLe32(sender->offset),
            .chunk_size_le = BleHciWire_ToLe16(chunkSize),
        };
        for (uint16_t index = 0U; index < chunkSize; index++) {
            acl.data[index] = TestTransfer_PatternByte(sender->offset + index);
        }
        const size_t h4Length = sizeof(acl.h4) + aclLength;
        const BleHciCrcTrailer_t trailer = {
            BleHciWire_ToBe16(AppH4_Crc16(&acl, h4Length))
        };
        const size_t wireLength = h4Length + sizeof(trailer);
        if (batchLength + wireLength > sizeof(wireBatch)) break;
        memcpy(&wireBatch[batchLength], &acl, h4Length);
        memcpy(&wireBatch[batchLength + h4Length], &trailer, sizeof(trailer));
        batchLength += wireLength;
        batchPackets++;
        sender->offset += chunkSize;
    }
    if (batchLength != 0U) {
        if (!AppSerialPort_Write(port, wireBatch, batchLength)) {
            sender->active = false;
            appendLog(log, "ERROR  Test transfer stopped after %u bytes",
                      (unsigned int)sender->offset);
            return;
        }
        log->transmittedPackets += batchPackets;
        if (sender->offset >= sender->nextProgress) {
            appendLog(log, "TEST  Sent %u/%u bytes (%u%%)",
                      (unsigned int)sender->offset, (unsigned int)sender->totalSize,
                      (unsigned int)((sender->offset * 100ULL) / sender->totalSize));
            sender->nextProgress += TEST_TRANSFER_PROGRESS_STEP;
        }
    }
    if (sender->offset == sender->totalSize) {
        appendLog(log, "TEST  All %u bytes sent; waiting for server verification",
                  (unsigned int)sender->totalSize);
        sender->active = false;
    }
}

/** Run the portable graphical client event loop. */
int main(void) {
    const int windowWidth = 1120;
    const int windowHeight = 960;
    char windowTitle[96];
    (void)snprintf(windowTitle, sizeof(windowTitle), "BLE UART Client v%s",
                   UART_DMA_APP_VERSION);
    SetConfigFlags(FLAG_WINDOW_HIGHDPI | FLAG_MSAA_4X_HINT);
    InitWindow(windowWidth, windowHeight, windowTitle);
    SetWindowMinSize(980, 820);
    SetTargetFPS(60);
    AppDashboardUi_Init(UART_DMA_UI_FONT_FILE, UART_DMA_UI_DISPLAY_FONT_FILE);

    AppSerialPort_t serial = {0};
    AppSerialPort_Init(&serial);
    AppH4StreamParser_t parser;
    AppH4StreamParser_Init(&parser);
    TrafficLog_t log = {0};
    AppDashboardLog_Init(&log.messages);
    TestSender_t testSender = {0U, 0U, 0U, 0U, false, 0.0};
    ConnectionHandshake_t handshake = {0};
    AppDashboardPipeline_t pipeline;
    AppDashboardPipeline_Reset(&pipeline);
    uint64_t producedSinceSample = 0U;
    uint64_t consumedSinceSample = 0U;
    ClientFrameContext_t frameContext = {
        &log, &handshake, &pipeline,
        &producedSinceSample, &consumedSinceSample
    };
#ifdef _WIN32
    char portText[128] = "COM3";
#elif defined(__APPLE__)
    char portText[128] = "/dev/ttys006";
#else
    char portText[128] = "/dev/ttyUSB0";
#endif
    (void)AppClientPreferences_LoadPort(portText, sizeof(portText));
    char opcodeText[16] = "0C03";
    char parametersText[768] = "";
    char rawText[1024] = "01 03 0C 00";
    char statusText[180] = "Disconnected";
    bool portEdit = false;
    bool opcodeEdit = false;
    bool parametersEdit = false;
    bool rawEdit = false;
    bool presetDropdown = false;
    bool bulkSizeDropdown = false;
    bool ringSizeDropdown = false;
    bool dmaSizeDropdown = false;
    bool clockDropdown = false;
    bool baudDropdown = false;
    int presetIndex = 0;
    int previousPreset = 0;
    AppDashboardSettings_t savedSettings;
    (void)AppClientPreferences_LoadSettings(
        APP_PREFERENCES_CLIENT, &savedSettings);
    int bulkSizeIndex = AppDashboardConfig_FindSize(
        APP_DASHBOARD_BULK_SIZES, APP_DASHBOARD_BULK_SIZE_COUNT,
        savedSettings.bulkBytes, 5);
    int ringSizeIndex = AppDashboardConfig_FindSize(
        APP_DASHBOARD_RING_SIZES, APP_DASHBOARD_RING_SIZE_COUNT,
        savedSettings.ringBytes, 6);
    int dmaSizeIndex = AppDashboardConfig_FindSize(
        APP_DASHBOARD_DMA_SIZES, APP_DASHBOARD_DMA_SIZE_COUNT,
        savedSettings.dmaBytes, 3);
    int clockIndex = AppDashboardConfig_FindSize(
        APP_DASHBOARD_SYSTEM_CLOCKS, APP_DASHBOARD_SYSTEM_CLOCK_COUNT,
        savedSettings.systemClockHz, 4);
    int baudIndex = AppDashboardConfig_FindSize(
        APP_DASHBOARD_BAUD_RATES, APP_DASHBOARD_BAUD_RATE_COUNT,
        savedSettings.baud, 4);
    savedSettings = (AppDashboardSettings_t){
        .systemClockHz = (uint32_t)APP_DASHBOARD_SYSTEM_CLOCKS[clockIndex].size,
        .baud = (uint32_t)APP_DASHBOARD_BAUD_RATES[baudIndex].size,
        .ringBytes = (uint32_t)APP_DASHBOARD_RING_SIZES[ringSizeIndex].size,
        .dmaBytes = (uint32_t)APP_DASHBOARD_DMA_SIZES[dmaSizeIndex].size,
        .bulkBytes = (uint32_t)APP_DASHBOARD_BULK_SIZES[bulkSizeIndex].size,
        .byteIrq = savedSettings.byteIrq,
    };
    log.byteIrqMode = savedSettings.byteIrq;
    log.receiveByteIrqActive = savedSettings.byteIrq;
    pipeline.queueCapacity = 16U;
    pipeline.ringCapacity =
        (uint32_t)APP_DASHBOARD_RING_SIZES[ringSizeIndex].size;
    pipeline.dmaCapacity =
        (uint32_t)APP_DASHBOARD_DMA_SIZES[dmaSizeIndex].size;
    AppLpuartSimulator_t receiverSimulator;
    (void)AppLpuartSim_Configure(
        &receiverSimulator,
        (uint32_t)APP_DASHBOARD_SYSTEM_CLOCKS[clockIndex].size,
        (uint32_t)APP_DASHBOARD_BAUD_RATES[baudIndex].size);
    double pipelineSampleTime = GetTime();
    AppDashboardConfig_PresetToFields(0, opcodeText, sizeof(opcodeText), parametersText, sizeof(parametersText));
    appendLog(&log, "Ready. Start ble_uart_server and enter its printed port.");

    while (!WindowShouldClose()) {
        if (serial.connected) {
            uint8_t incoming[4096U];
            int received = 0;
            do {
                /* Drain every completed DMA transaction already waiting in the
                 * nonblocking driver. Ring capacity limits one IDLE-delimited
                 * frame; it must not become a per-render-frame byte budget. */
                const size_t readCapacity =
                    APP_DASHBOARD_DMA_SIZES[dmaSizeIndex].size;
                received = AppSerialPort_Read(
                    &serial, incoming, readCapacity);
                if (received <= 0) {
                    break;
                }
                const double queuedAt = GetTime();
                (void)AppH4StreamParser_Feed(
                    &parser, incoming, (size_t)received,
                    logFrame, &frameContext);
                pipeline.latencyUs = (uint64_t)((GetTime() - queuedAt) * 1000000.0);
                if (pipeline.latencyUs > pipeline.latencyMaxUs) {
                    pipeline.latencyMaxUs = pipeline.latencyUs;
                }
            } while (received > 0);
            if (received < 0) {
                appendLog(&log, "ERROR  Serial connection lost");
                AppSerialPort_Close(&serial);
                (void)snprintf(statusText, sizeof(statusText), "Disconnected after read error");
                handshake.pending = false;
                handshake.verified = false;
            }
        }
        if (serial.connected && handshake.failed) {
            AppSerialPort_Close(&serial);
            handshake.failed = false;
            handshake.verified = false;
            (void)snprintf(statusText, sizeof(statusText),
                           "Wrong UART: BLE UART server acknowledgement did not match");
            appendLog(&log, "ERROR  Selected UART is not a BLE UART server");
        } else if (serial.connected && handshake.pending &&
                   GetTime() >= handshake.deadline) {
            AppSerialPort_Close(&serial);
            handshake.pending = false;
            handshake.verified = false;
            (void)snprintf(statusText, sizeof(statusText),
                           "Wrong UART: BLE UART server did not acknowledge probe");
            appendLog(&log, "ERROR  Selected UART is not a BLE UART server (timeout)");
        } else if (serial.connected && handshake.verified &&
                   !handshake.readyAnnounced) {
            handshake.readyAnnounced = true;
            (void)snprintf(statusText, sizeof(statusText), "Connected to %s", portText);
            appendLog(&log, "Connected to %s; server identity verified", portText);
        }
        pumpTestTransfer(&testSender, &serial, &log,
                         (uint32_t)APP_DASHBOARD_BAUD_RATES[baudIndex].size);
        if (!serial.connected) {
            testSender.active = false;
            log.testIrqActive = false;
        }

        const double sampleNow = GetTime();
        if (sampleNow - pipelineSampleTime >= 0.5) {
            const double elapsed = sampleNow - pipelineSampleTime;
            pipeline.producerBps = (uint64_t)((double)producedSinceSample / elapsed);
            pipeline.consumerBps = (uint64_t)((double)consumedSinceSample / elapsed);
            pipeline.queueDepth = 0U;
            pipeline.queueCapacity = 16U;
            pipeline.queuedBytes = 0U;
            pipeline.ringCapacity =
                (uint32_t)APP_DASHBOARD_RING_SIZES[ringSizeIndex].size;
            pipeline.dmaCapacity =
                (uint32_t)APP_DASHBOARD_DMA_SIZES[dmaSizeIndex].size;
            if (pipeline.producerBps > pipeline.consumerBps) {
                const uint64_t growth = pipeline.producerBps - pipeline.consumerBps;
                const uint64_t freeBytes = pipeline.ringCapacity > pipeline.ringBurstBytes
                    ? pipeline.ringCapacity - pipeline.ringBurstBytes : 0U;
                pipeline.headroomUs = freeBytes * 1000000ULL / growth;
            } else {
                pipeline.headroomUs = UINT64_MAX;
            }
            AppDashboardPipeline_Sample(&pipeline);
            producedSinceSample = 0U;
            consumedSinceSample = 0U;
            pipelineSampleTime = sampleNow;
        }

        /* Raygui text boxes have caller-owned focus; keep exactly one active. */
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            portEdit = false;
            opcodeEdit = false;
            parametersEdit = false;
            rawEdit = false;
        }

        BeginDrawing();
        ClearBackground(COLOR_CANVAS);
        char heading[96];
        (void)snprintf(heading, sizeof(heading), "BLE UART HCI Client  v%s",
                       UART_DMA_APP_VERSION);
        AppDashboardUi_DrawDisplayText(heading, 28, 22, 28, COLOR_TEXT);
        AppDashboardUi_DrawText("PORTABLE H4 CONSOLE  /  CRC16-CCITT  /  LIVE PACKET TELEMETRY",
                   30, 62, 12, COLOR_MUTED);

        char txMetric[32];
        char rxMetric[32];
        char irqMetric[32];
        (void)snprintf(txMetric, sizeof(txMetric), "%u",
                       (unsigned int)log.transmittedPackets);
        (void)snprintf(rxMetric, sizeof(rxMetric), "%u",
                       (unsigned int)log.receivedPackets);
        (void)snprintf(irqMetric, sizeof(irqMetric), "%u",
                       (unsigned int)log.absoluteIrqCount);
        if (AppDashboardUi_MetricCard((Rectangle){24, 100, 259, 84}, "RX PACKETS",
                           rxMetric, COLOR_BLUE)) {
            log.receivedPackets = 0U;
        }
        if (AppDashboardUi_MetricCard((Rectangle){295, 100, 259, 84}, "TX PACKETS",
                           txMetric, COLOR_CYAN)) {
            log.transmittedPackets = 0U;
        }
        if (AppDashboardUi_MetricCard((Rectangle){566, 100, 259, 84}, "RX IRQ CALLS",
                           irqMetric, COLOR_PURPLE)) {
            log.absoluteIrqCount = 0U;
        }
        (void)AppDashboardUi_MetricCard((Rectangle){837, 100, 259, 84}, "TRANSPORT",
                             handshake.pending ? "VERIFYING" : serial.connected
                                 ? (log.byteIrqMode ? "BYTE IRQ" : "DMA + IDLE")
                                 : "OFFLINE",
                             serial.connected ? COLOR_CYAN : COLOR_MUTED);

        AppDashboardUi_DrawPanel((Rectangle){24, 204, 1072, 136}, "CONNECTION");
        GuiLabel((Rectangle){44, 247, 50, 38}, "Port");
        AppDashboardUi_TextBox((Rectangle){96, 247, 426, 38}, portText,
                             sizeof(portText), &portEdit);
        if (AppDashboardUi_Button((Rectangle){756, 247, 170, 38},
                         serial.connected ? "Disconnect" : "Connect", true)) {
            if (serial.connected) {
                AppSerialPort_Close(&serial);
                handshake.pending = false;
                handshake.verified = false;
                (void)snprintf(statusText, sizeof(statusText), "Disconnected");
                appendLog(&log, "Disconnected from %s", portText);
            } else {
                const unsigned long baud =
                    APP_DASHBOARD_BAUD_RATES[baudIndex].size;
                char error[180];
                (void)AppClientPreferences_SavePort(portText);
                if (AppSerialPort_Open(&serial, portText, baud, error, sizeof(error))) {
                    AppH4StreamParser_Init(&parser);
                    AppDashboardPipeline_Reset(&pipeline);
                    pipeline.queueCapacity = 16U;
                    pipeline.ringCapacity =
                        (uint32_t)APP_DASHBOARD_RING_SIZES[ringSizeIndex].size;
                    pipeline.dmaCapacity =
                        (uint32_t)APP_DASHBOARD_DMA_SIZES[dmaSizeIndex].size;
                    producedSinceSample = 0U;
                    consumedSinceSample = 0U;
                    pipelineSampleTime = GetTime();
                    (void)snprintf(statusText, sizeof(statusText),
                                   "Verifying BLE UART server on %s...", portText);
                    appendLog(&log, "Opened %s at %lu baud; verifying server", portText, baud);
                    if (!sendConnectionProbe(&serial, &log, &handshake)) {
                        AppSerialPort_Close(&serial);
                        (void)snprintf(statusText, sizeof(statusText),
                                       "Could not send BLE UART verification probe");
                    }
                } else {
                    (void)snprintf(statusText, sizeof(statusText), "%s", error);
                    appendLog(&log, "ERROR  %s", error);
                }
            }
        }
        AppDashboardUi_StatusField((Rectangle){44, 292, 166, 34}, statusText);
        AppDashboardUi_DrawText("Clock", 230, 300, 12, COLOR_MUTED);
        AppDashboardUi_DrawText("Baud", 390, 300, 12, COLOR_MUTED);
        AppDashboardUi_DrawText("Ring", 548, 300, 12, COLOR_MUTED);
        AppDashboardUi_DrawText("DMA", 706, 300, 12, COLOR_MUTED);
        const bool restoreDefaults = AppDashboardUi_Button(
            (Rectangle){858, 292, 90, 34}, "Defaults", true);
        const bool connectionReady = serial.connected && handshake.verified;
        const bool sendControlsReady = connectionReady && !testSender.active &&
            !log.testIrqActive;
        const bool rxModeChanged = AppDashboardUi_Toggle(
            (Rectangle){958, 254, 118, 24}, "Per-byte IRQ",
            &log.byteIrqMode, connectionReady);
        if (rxModeChanged) {
            log.receiveByteIrqActive = log.byteIrqMode;
            appendLog(&log, "RX  Local receive mode: %s",
                      log.byteIrqMode ? "per-byte IRQ" : "DMA batches");
        }

        AppDashboardUi_DrawPanel((Rectangle){24, 360, 1072, 216}, "HCI COMMAND BUILDER");
        GuiLabel((Rectangle){44, 397, 64, 38}, "Preset");
        if (presetIndex != previousPreset) {
            AppDashboardConfig_PresetToFields(presetIndex, opcodeText, sizeof(opcodeText),
                           parametersText, sizeof(parametersText));
            previousPreset = presetIndex;
        }
        GuiLabel((Rectangle){496, 397, 66, 38}, "Opcode");
        AppDashboardUi_TextBox((Rectangle){566, 397, 164, 38}, opcodeText,
                             sizeof(opcodeText), &opcodeEdit);
        GuiLabel((Rectangle){44, 452, 132, 38}, "Parameters");
        AppDashboardUi_TextBox((Rectangle){180, 452, 646, 38}, parametersText,
                             sizeof(parametersText), &parametersEdit);
        if (AppDashboardUi_Button((Rectangle){850, 452, 226, 38}, "Send command",
                         sendControlsReady)) {
            char *end = NULL;
            const unsigned long opcode = strtoul(opcodeText, &end, 16);
            uint8_t parameters[255];
            const size_t parameterLength = AppH4_ParseHex(parametersText, parameters, sizeof(parameters));
            if (end == opcodeText || *end != '\0' || opcode > 0xFFFFUL) {
                appendLog(&log, "ERROR  Invalid 16-bit hexadecimal opcode");
            } else if (parameterLength == SIZE_MAX) {
                appendLog(&log, "ERROR  Invalid hexadecimal parameter bytes");
            } else {
                (void)sendCommand(&serial, &log, (uint16_t)opcode,
                                  parameters, parameterLength);
            }
        }
        GuiLabel((Rectangle){44, 512, 132, 38}, "Raw H4 bytes");
        AppDashboardUi_TextBox((Rectangle){180, 512, 646, 38}, rawText,
                             sizeof(rawText), &rawEdit);
        if (AppDashboardUi_Button((Rectangle){850, 512, 226, 38}, "Append CRC + send",
                         sendControlsReady)) {
            uint8_t raw[APP_H4_MAX_FRAME_SIZE - 2U];
            const size_t rawLength = AppH4_ParseHex(rawText, raw, sizeof(raw));
            if (rawLength == SIZE_MAX || rawLength == 0U) {
                appendLog(&log, "ERROR  Enter valid raw H4 hexadecimal bytes");
            } else {
                (void)sendFramed(&serial, &log, raw, rawLength);
            }
        }

        AppDashboardUi_DrawPanel((Rectangle){24, 596, 1072, 124}, "TEST TRANSMISSIONS");
        if (AppDashboardUi_Button((Rectangle){44, 638, 220, 42}, "Send 128-byte packet",
                         sendControlsReady)) {
            startTestTransfer(&testSender, &serial, &log, 128U);
        }
        GuiLabel((Rectangle){292, 640, 92, 38}, "Block size");
        if (AppDashboardUi_Button((Rectangle){548, 638, 218, 42}, "Send selected block",
                         sendControlsReady)) {
            startTestTransfer(&testSender, &serial, &log,
                              APP_DASHBOARD_BULK_SIZES[bulkSizeIndex].size);
        }
        const float progress = testSender.totalSize == 0U ? 0.0f
            : (float)testSender.offset / (float)testSender.totalSize;
        DrawRectangleRounded((Rectangle){792, 638, 284, 16}, 1.0f, 12,
                             (Color){35, 45, 65, 255});
        DrawRectangleRounded((Rectangle){792, 638, 284.0f * progress, 16},
                             1.0f, 12, COLOR_BLUE);
        char progressText[96];
        (void)snprintf(progressText, sizeof(progressText), "%u / %u bytes",
                       (unsigned int)testSender.offset,
                       (unsigned int)testSender.totalSize);
        AppDashboardUi_DrawText(progressText, 792, 665, 12, COLOR_MUTED);

        AppDashboardUi_DrawPanel((Rectangle){24, 740, 1072, 196},
                                 "LIVE UART TRAFFIC + PIPELINE HEADROOM");
        DrawRectangleRounded((Rectangle){44, 777, 540, 142}, 16.0f / 142.0f, 12,
                             (Color){7, 11, 20, 255});
        AppDashboardLog_Draw(&log.messages,
                             (Rectangle){58, 786, 512, 135},
                             AppDashboardUi_RegularFont(12),
                             (float)AppDashboardUi_RenderSize(12),
                             15.0f, clientLogColor);
        AppDashboardPipeline_Draw(&pipeline, (Rectangle){600, 777, 476, 142});

        /* Draw last so the expanded selector overlays the traffic panel. */
        (void)AppDashboard_UpwardDropdown(
            (Rectangle){112, 397, 360, 38}, AppDashboardUi_Font(), APP_DASHBOARD_PRESET_OPTIONS,
            APP_DASHBOARD_PRESET_COUNT,
            &presetIndex, &presetDropdown);
        const bool bulkSizeChanged = AppDashboard_UpwardDropdown(
            (Rectangle){392, 638, 132, 42}, AppDashboardUi_Font(), APP_DASHBOARD_BULK_SIZE_OPTIONS,
            APP_DASHBOARD_BULK_SIZE_COUNT,
            &bulkSizeIndex, &bulkSizeDropdown);
        const bool ringSizeChanged = AppDashboard_UpwardDropdown(
            (Rectangle){582, 292, 104, 34}, AppDashboardUi_Font(), APP_DASHBOARD_RING_SIZE_OPTIONS,
            APP_DASHBOARD_RING_SIZE_COUNT,
            &ringSizeIndex, &ringSizeDropdown);
        const bool dmaSizeChanged = AppDashboard_UpwardDropdown(
            (Rectangle){742, 292, 104, 34}, AppDashboardUi_Font(), APP_DASHBOARD_DMA_SIZE_OPTIONS,
            APP_DASHBOARD_DMA_SIZE_COUNT,
            &dmaSizeIndex, &dmaSizeDropdown);
        const bool clockChanged = AppDashboard_UpwardDropdown(
            (Rectangle){276, 292, 104, 34}, AppDashboardUi_Font(),
            APP_DASHBOARD_SYSTEM_CLOCK_OPTIONS, APP_DASHBOARD_SYSTEM_CLOCK_COUNT,
            &clockIndex, &clockDropdown);
        const bool baudChanged = AppDashboard_UpwardDropdown(
            (Rectangle){430, 292, 104, 34}, AppDashboardUi_Font(),
            APP_DASHBOARD_BAUD_RATE_OPTIONS, APP_DASHBOARD_BAUD_RATE_COUNT,
            &baudIndex, &baudDropdown);
        if (clockChanged || baudChanged) {
            (void)AppLpuartSim_Configure(
                &receiverSimulator,
                (uint32_t)APP_DASHBOARD_SYSTEM_CLOCKS[clockIndex].size,
                (uint32_t)APP_DASHBOARD_BAUD_RATES[baudIndex].size);
            appendLog(&log, "RX  Timing: clock=%u baud=%u OSR=%u SBR=%u actual=%u",
                      receiverSimulator.sourceClockHz,
                      receiverSimulator.requestedBaud, receiverSimulator.osr,
                      receiverSimulator.sbr, receiverSimulator.actualBaud);
        }
        if (ringSizeChanged || dmaSizeChanged) {
            pipeline.ringCapacity =
                (uint32_t)APP_DASHBOARD_RING_SIZES[ringSizeIndex].size;
            pipeline.dmaCapacity =
                (uint32_t)APP_DASHBOARD_DMA_SIZES[dmaSizeIndex].size;
            pipeline.ringBurstBytes = 0U;
            pipeline.ringHighWaterBytes = 0U;
            pipeline.dmaBurstBytes = 0U;
            pipeline.dmaHighWaterBytes = 0U;
            appendLog(&log, "RX  Buffers: ring=%u bytes, DMA=%u bytes",
                      (unsigned int)APP_DASHBOARD_RING_SIZES[ringSizeIndex].size,
                      (unsigned int)APP_DASHBOARD_DMA_SIZES[dmaSizeIndex].size);
        }
        if (restoreDefaults) {
            AppClientPreferences_DefaultSettings(&savedSettings);
            bulkSizeIndex = 5;
            ringSizeIndex = 6;
            dmaSizeIndex = 3;
            clockIndex = 4;
            baudIndex = 4;
            log.byteIrqMode = false;
            log.receiveByteIrqActive = false;
            AppDashboardPipeline_Reset(&pipeline);
            pipeline.queueCapacity = 16U;
            pipeline.ringCapacity = (uint32_t)savedSettings.ringBytes;
            pipeline.dmaCapacity = (uint32_t)savedSettings.dmaBytes;
            producedSinceSample = 0U;
            consumedSinceSample = 0U;
            pipelineSampleTime = GetTime();
            (void)AppLpuartSim_Configure(
                &receiverSimulator, savedSettings.systemClockHz,
                savedSettings.baud);
            appendLog(&log, "SETTINGS  Restored application defaults");
        }
        if (restoreDefaults || bulkSizeChanged || ringSizeChanged ||
            dmaSizeChanged || clockChanged || baudChanged || rxModeChanged) {
            savedSettings = (AppDashboardSettings_t){
                .systemClockHz = (uint32_t)APP_DASHBOARD_SYSTEM_CLOCKS[clockIndex].size,
                .baud = (uint32_t)APP_DASHBOARD_BAUD_RATES[baudIndex].size,
                .ringBytes = (uint32_t)APP_DASHBOARD_RING_SIZES[ringSizeIndex].size,
                .dmaBytes = (uint32_t)APP_DASHBOARD_DMA_SIZES[dmaSizeIndex].size,
                .bulkBytes = (uint32_t)APP_DASHBOARD_BULK_SIZES[bulkSizeIndex].size,
                .byteIrq = log.byteIrqMode,
            };
            (void)AppClientPreferences_SaveSettings(
                APP_PREFERENCES_CLIENT, &savedSettings);
        }
        EndDrawing();
    }

    AppSerialPort_Close(&serial);
    (void)AppClientPreferences_SavePort(portText);
    (void)AppClientPreferences_SaveSettings(
        APP_PREFERENCES_CLIENT, &savedSettings);
    AppDashboardUi_Shutdown();
    CloseWindow();
    return 0;
}
