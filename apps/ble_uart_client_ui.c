/** @file
 * @brief Portable raylib/raygui BLE HCI UART client application. */
#include "raylib.h"
#include "h4_wire.h"
#include "serial_port.h"
#include "test_transfer_protocol.h"

#define RAYGUI_IMPLEMENTATION
#include "raygui.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_LOG_LINES       13U
#define MAX_LOG_LENGTH      220U

/** HCI command preset displayed in the graphical command picker. */
typedef struct {
    /** Human-readable label. */
    const char *name;
    /** Command opcode in host byte order. */
    uint16_t opcode;
    /** Preset parameter bytes. */
    uint8_t parameters[8];
    /** Valid parameter count. */
    size_t parameterLength;
} Preset_t;

/** Fixed-height rolling traffic log displayed by the UI. */
typedef struct {
    /** Render-ready text lines. */
    char lines[MAX_LOG_LINES][MAX_LOG_LENGTH];
    /** Number of populated lines. */
    size_t count;
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
} TestSender_t;

static const Preset_t presets[] = {
    {"HCI Reset",          0x0C03U, {0U},            0U},
    {"Read Local Version", 0x1001U, {0U},            0U},
    {"Read BD_ADDR",       0x1009U, {0U},            0U},
    {"LE Read Buffer Size",0x2002U, {0U},            0U},
    {"LE Enable Scan",     0x200CU, {0x01U, 0x00U}, 2U},
    {"LE Disable Scan",    0x200CU, {0x00U, 0x00U}, 2U},
};

/** Append a formatted line, evicting the oldest line when full. */
static void appendLog(TrafficLog_t *log, const char *format, ...) {
    if (log->count == MAX_LOG_LINES) {
        memmove(log->lines, &log->lines[1],
                (MAX_LOG_LINES - 1U) * sizeof(log->lines[0]));
        log->count--;
    }
    va_list arguments;
    va_start(arguments, format);
    (void)vsnprintf(log->lines[log->count], MAX_LOG_LENGTH, format, arguments);
    va_end(arguments);
    log->count++;
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
    char hex[MAX_LOG_LENGTH - 24U];
    AppH4_FormatHex(hex, sizeof(hex), h4Packet, h4Length);
    if (!AppSerialPort_Write(port, h4Packet, h4Length) ||
        !AppSerialPort_Write(port, &trailer, sizeof(trailer))) {
        appendLog(log, "ERROR  Serial write failed");
        return false;
    }
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

/** Convert a received frame callback into a traffic-log entry. */
static void logFrame(const uint8_t *frame, size_t length, bool crcValid,
                     void *context) {
    TrafficLog_t *const log = context;
    const size_t h4Length = length - 2U;
    char hex[MAX_LOG_LENGTH - 56U];
    AppH4_FormatHex(hex, sizeof(hex), frame, length);
    BleHciPacketIndicator_t indicator;
    memcpy(&indicator, frame, sizeof(indicator));
    if (indicator.value == APP_H4_EVENT && h4Length >= sizeof(BleHciEventHeader_t)) {
        BleHciEventHeader_t header;
        memcpy(&header, frame, sizeof(header));
        appendLog(log, "RX  %s  [%s, Event %02X]", hex,
                  crcValid ? "CRC OK" : "CRC BAD", header.fields.event_code);
    } else {
        appendLog(log, "RX  %s  [%s]", hex, crcValid ? "CRC OK" : "CRC BAD");
    }
}

/** Copy a selected command preset into editable text fields. */
static void presetToFields(int presetIndex, char *opcode, size_t opcodeSize,
                           char *parameters, size_t parameterSize) {
    const Preset_t *preset = &presets[presetIndex];
    (void)snprintf(opcode, opcodeSize, "%04X", preset->opcode);
    AppH4_FormatHex(parameters, parameterSize, preset->parameters, preset->parameterLength);
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
    appendLog(log, "TEST  Sending %u bytes (id=%u)",
              (unsigned int)totalSize, sender->identifier);
}

/** Generate and send a bounded batch of test chunks during one render frame. */
static void pumpTestTransfer(TestSender_t *sender, AppSerialPort_t *port,
                             TrafficLog_t *log) {
    if (!sender->active) {
        return;
    }
    for (unsigned int packet = 0U; packet < 32U && sender->offset < sender->totalSize; packet++) {
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
        if (!sendFramedEx(port, log, &acl, sizeof(acl.h4) + aclLength, false)) {
            sender->active = false;
            appendLog(log, "ERROR  Test transfer stopped after %u bytes",
                      (unsigned int)sender->offset);
            return;
        }
        sender->offset += chunkSize;
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
    const int windowWidth = 960;
    const int windowHeight = 780;
    InitWindow(windowWidth, windowHeight, "BLE UART Client");
    SetWindowMinSize(760, 600);
    SetTargetFPS(60);

    AppSerialPort_t serial = {0};
    AppSerialPort_Init(&serial);
    AppH4StreamParser_t parser;
    AppH4StreamParser_Init(&parser);
    TrafficLog_t log = {{{0}}, 0U};
    TestSender_t testSender = {0U, 0U, 0U, 0U, false};
#ifdef _WIN32
    char portText[128] = "COM3";
#elif defined(__APPLE__)
    char portText[128] = "/dev/ttys006";
#else
    char portText[128] = "/dev/ttyUSB0";
#endif
    char baudText[32] = "4000000";
    char opcodeText[16] = "0C03";
    char parametersText[768] = "";
    char rawText[1024] = "01 03 0C 00";
    char statusText[180] = "Disconnected";
    bool portEdit = false;
    bool baudEdit = false;
    bool opcodeEdit = false;
    bool parametersEdit = false;
    bool rawEdit = false;
    bool presetDropdown = false;
    int presetIndex = 0;
    int previousPreset = 0;
    presetToFields(0, opcodeText, sizeof(opcodeText), parametersText, sizeof(parametersText));
    appendLog(&log, "Ready. Start ble_uart_server and enter its printed port.");

    while (!WindowShouldClose()) {
        if (serial.connected) {
            uint8_t incoming[512];
            const int received = AppSerialPort_Read(&serial, incoming, sizeof(incoming));
            if (received > 0) {
                (void)AppH4StreamParser_Feed(&parser, incoming, (size_t)received,
                                             logFrame, &log);
            } else if (received < 0) {
                appendLog(&log, "ERROR  Serial connection lost");
                AppSerialPort_Close(&serial);
                (void)snprintf(statusText, sizeof(statusText), "Disconnected after read error");
            }
        }
        pumpTestTransfer(&testSender, &serial, &log);

        BeginDrawing();
        ClearBackground((Color){245, 247, 250, 255});
        DrawText("BLE UART HCI Client", 24, 20, 26, (Color){25, 35, 55, 255});
        DrawText("Portable C UI • H4 framing • CRC16-CCITT • live receive parser",
                 24, 52, 16, (Color){90, 100, 120, 255});

        GuiGroupBox((Rectangle){20, 84, 920, 106}, "Serial connection");
        GuiLabel((Rectangle){36, 103, 60, 24}, "Port");
        if (GuiTextBox((Rectangle){86, 101, 410, 30}, portText, sizeof(portText), portEdit)) {
            portEdit = !portEdit;
        }
        GuiLabel((Rectangle){516, 103, 50, 24}, "Baud");
        if (GuiTextBox((Rectangle){563, 101, 125, 30}, baudText, sizeof(baudText), baudEdit)) {
            baudEdit = !baudEdit;
        }
        if (GuiButton((Rectangle){706, 101, 210, 30}, serial.connected ? "Disconnect" : "Connect")) {
            if (serial.connected) {
                AppSerialPort_Close(&serial);
                (void)snprintf(statusText, sizeof(statusText), "Disconnected");
                appendLog(&log, "Disconnected from %s", portText);
            } else {
                char *end = NULL;
                const unsigned long baud = strtoul(baudText, &end, 10);
                char error[180];
                if (end == baudText || *end != '\0') {
                    (void)snprintf(statusText, sizeof(statusText), "Invalid baud rate");
                } else if (AppSerialPort_Open(&serial, portText, baud, error, sizeof(error))) {
                    (void)snprintf(statusText, sizeof(statusText), "Connected to %s", portText);
                    appendLog(&log, "Connected to %s at %lu baud", portText, baud);
                } else {
                    (void)snprintf(statusText, sizeof(statusText), "%s", error);
                    appendLog(&log, "ERROR  %s", error);
                }
            }
        }
        GuiStatusBar((Rectangle){36, 145, 880, 26}, statusText);
        DrawText("Hardware baud rates supported up to 4000000",
                 563, 134, 11, (Color){100, 110, 128, 255});

        GuiGroupBox((Rectangle){20, 205, 920, 176}, "HCI command");
        GuiLabel((Rectangle){36, 225, 65, 24}, "Preset");
        if (GuiDropdownBox((Rectangle){103, 222, 280, 30},
                           "HCI Reset;Read Local Version;Read BD_ADDR;LE Read Buffer Size;LE Enable Scan;LE Disable Scan",
                           &presetIndex, presetDropdown)) {
            presetDropdown = !presetDropdown;
        }
        if (presetIndex != previousPreset) {
            presetToFields(presetIndex, opcodeText, sizeof(opcodeText),
                           parametersText, sizeof(parametersText));
            previousPreset = presetIndex;
        }
        GuiLabel((Rectangle){408, 225, 60, 24}, "Opcode");
        if (GuiTextBox((Rectangle){470, 222, 120, 30}, opcodeText, sizeof(opcodeText), opcodeEdit)) {
            opcodeEdit = !opcodeEdit;
        }
        GuiLabel((Rectangle){36, 270, 110, 24}, "Parameters (hex)");
        if (GuiTextBox((Rectangle){148, 267, 598, 30}, parametersText,
                       sizeof(parametersText), parametersEdit)) {
            parametersEdit = !parametersEdit;
        }
        if (GuiButton((Rectangle){764, 267, 152, 30}, "Send command")) {
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
        GuiLabel((Rectangle){36, 320, 110, 24}, "Raw H4 (hex)");
        if (GuiTextBox((Rectangle){148, 317, 598, 30}, rawText, sizeof(rawText), rawEdit)) {
            rawEdit = !rawEdit;
        }
        if (GuiButton((Rectangle){764, 317, 152, 30}, "Append CRC + send")) {
            uint8_t raw[APP_H4_MAX_FRAME_SIZE - 2U];
            const size_t rawLength = AppH4_ParseHex(rawText, raw, sizeof(raw));
            if (rawLength == SIZE_MAX || rawLength == 0U) {
                appendLog(&log, "ERROR  Enter valid raw H4 hexadecimal bytes");
            } else {
                (void)sendFramed(&serial, &log, raw, rawLength);
            }
        }

        GuiGroupBox((Rectangle){20, 398, 920, 100}, "Bulk test messages");
        if (GuiButton((Rectangle){36, 422, 210, 34}, "Send 128-byte message")) {
            startTestTransfer(&testSender, &serial, &log, 128U);
        }
        if (GuiButton((Rectangle){262, 422, 210, 34}, "Send 2 MiB chunk")) {
            startTestTransfer(&testSender, &serial, &log, 2U * 1024U * 1024U);
        }
        const float progress = testSender.totalSize == 0U ? 0.0f
            : (float)testSender.offset / (float)testSender.totalSize;
        DrawRectangle(494, 428, 420, 20, (Color){220, 225, 232, 255});
        DrawRectangle(494, 428, (int)(420.0f * progress), 20,
                      (Color){70, 145, 235, 255});
        DrawRectangleLines(494, 428, 420, 20, (Color){150, 158, 172, 255});
        char progressText[96];
        (void)snprintf(progressText, sizeof(progressText), "%u / %u bytes",
                       (unsigned int)testSender.offset,
                       (unsigned int)testSender.totalSize);
        DrawText(progressText, 494, 460, 14, (Color){80, 90, 108, 255});

        GuiGroupBox((Rectangle){20, 514, 920, 246}, "UART traffic");
        DrawRectangle(34, 537, 892, 208, (Color){20, 25, 35, 255});
        for (size_t index = 0U; index < log.count; index++) {
            const Color color = strncmp(log.lines[index], "ERROR", 5U) == 0
                ? (Color){255, 120, 120, 255}
                : strncmp(log.lines[index], "TX", 2U) == 0
                    ? (Color){100, 190, 255, 255}
                    : strncmp(log.lines[index], "RX", 2U) == 0
                        ? (Color){105, 225, 150, 255}
                        : (Color){190, 198, 210, 255};
            DrawText(log.lines[index], 46, 547 + (int)index * 15, 13, color);
        }
        EndDrawing();
    }

    AppSerialPort_Close(&serial);
    CloseWindow();
    return 0;
}
