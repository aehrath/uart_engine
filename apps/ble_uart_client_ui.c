/** @file
 * @brief Portable raylib/raygui BLE HCI UART client application. */
#include "raylib.h"
#include "dashboard_dropdown.h"
#include "h4_wire.h"
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

#define MAX_LOG_LINES       9U
#define MAX_LOG_LENGTH      220U
#define CONNECTION_PROBE_SIZE 128U
#define CONNECTION_PROBE_TIMEOUT_SECONDS 2.5

static Font uiFont;
static Font displayFont;
static bool customUiFont;
static bool customDisplayFont;

static const Color COLOR_CANVAS = {10, 15, 28, 255};
static const Color COLOR_PANEL = {20, 28, 46, 255};
static const Color COLOR_PANEL_RAISED = {27, 37, 59, 255};
static const Color COLOR_BORDER = {49, 65, 92, 255};
static const Color COLOR_TEXT = {232, 239, 249, 255};
static const Color COLOR_MUTED = {143, 158, 181, 255};
static const Color COLOR_CYAN = {45, 212, 191, 255};
static const Color COLOR_BLUE = {76, 148, 255, 255};
static const Color COLOR_PURPLE = {167, 139, 250, 255};

/** Load the bundled sans-serif UI font and configure raygui to use it. */
static void loadUiFont(void) {
    const char *path = TextFormat("%s%s", GetApplicationDirectory(),
                                  UART_DMA_UI_FONT_FILE);
    uiFont = LoadFontEx(path, 36, NULL, 0);
    customUiFont = uiFont.texture.id != 0U;
    if (!customUiFont) {
        uiFont = GetFontDefault();
    }
    SetTextureFilter(uiFont.texture, TEXTURE_FILTER_BILINEAR);
    GuiSetFont(uiFont);
    GuiSetStyle(DEFAULT, TEXT_SIZE, 17);
    GuiSetStyle(DEFAULT, BORDER_WIDTH, 1);
    GuiSetStyle(DEFAULT, TEXT_PADDING, 10);
    GuiSetStyle(DEFAULT, BORDER_COLOR_NORMAL, ColorToInt(COLOR_BORDER));
    GuiSetStyle(DEFAULT, BASE_COLOR_NORMAL, ColorToInt(COLOR_PANEL_RAISED));
    GuiSetStyle(DEFAULT, TEXT_COLOR_NORMAL, ColorToInt(COLOR_TEXT));
    GuiSetStyle(DEFAULT, BORDER_COLOR_FOCUSED, ColorToInt(COLOR_CYAN));
    GuiSetStyle(DEFAULT, BASE_COLOR_FOCUSED, ColorToInt((Color){35, 50, 76, 255}));
    GuiSetStyle(DEFAULT, TEXT_COLOR_FOCUSED, ColorToInt(WHITE));
    GuiSetStyle(DEFAULT, BORDER_COLOR_PRESSED, ColorToInt(COLOR_BLUE));
    GuiSetStyle(DEFAULT, BASE_COLOR_PRESSED, ColorToInt((Color){42, 63, 96, 255}));
    GuiSetStyle(DEFAULT, TEXT_COLOR_PRESSED, ColorToInt(WHITE));
    GuiSetStyle(DEFAULT, BORDER_COLOR_DISABLED, ColorToInt((Color){43, 52, 70, 255}));
    GuiSetStyle(DEFAULT, BASE_COLOR_DISABLED, ColorToInt((Color){24, 30, 43, 255}));
    GuiSetStyle(DEFAULT, TEXT_COLOR_DISABLED, ColorToInt((Color){91, 104, 124, 255}));
    GuiSetStyle(DROPDOWNBOX, BASE_COLOR_NORMAL, ColorToInt((Color){14, 22, 38, 255}));
    GuiSetStyle(DROPDOWNBOX, TEXT_COLOR_NORMAL, ColorToInt((Color){241, 246, 255, 255}));
    GuiSetStyle(DROPDOWNBOX, BASE_COLOR_FOCUSED, ColorToInt((Color){103, 232, 249, 255}));
    GuiSetStyle(DROPDOWNBOX, TEXT_COLOR_FOCUSED, ColorToInt((Color){7, 22, 28, 255}));
    GuiSetStyle(DROPDOWNBOX, BASE_COLOR_PRESSED, ColorToInt(COLOR_BLUE));
    GuiSetStyle(DROPDOWNBOX, TEXT_COLOR_PRESSED, ColorToInt(WHITE));

    const char *displayPath = TextFormat("%s%s", GetApplicationDirectory(),
                                         UART_DMA_UI_DISPLAY_FONT_FILE);
    displayFont = LoadFontEx(displayPath, 42, NULL, 0);
    customDisplayFont = displayFont.texture.id != 0U;
    if (!customDisplayFont) {
        displayFont = uiFont;
    }
    SetTextureFilter(displayFont.texture, TEXTURE_FILTER_BILINEAR);
}

/** Draw antialiased text using the shared UI typeface. */
static void drawUiText(const char *text, int x, int y, int size, Color color) {
    DrawTextEx(uiFont, text, (Vector2){(float)x, (float)y}, (float)size, 0.0f, color);
}

/** Draw title and metric text using the technical display face. */
static void drawDisplayText(const char *text, int x, int y, int size, Color color) {
    DrawTextEx(displayFont, text, (Vector2){(float)x, (float)y},
               (float)size, 0.5f, color);
}

/** Draw one rounded dashboard panel with an optional section title. */
static void drawPanel(Rectangle bounds, const char *title) {
    DrawRectangleRounded(bounds, 0.08f, 10, COLOR_PANEL);
    DrawRectangleRoundedLinesEx(bounds, 0.08f, 10, 1.0f, COLOR_BORDER);
    if (title != NULL) {
        drawDisplayText(title, (int)bounds.x + 20, (int)bounds.y + 15,
                        16, COLOR_MUTED);
    }
}

/** Draw one client telemetry card. */
static void drawMetricCard(Rectangle bounds, const char *label,
                           const char *value, Color accent) {
    DrawRectangleRounded(bounds, 0.12f, 10, COLOR_PANEL_RAISED);
    DrawRectangleRounded((Rectangle){bounds.x, bounds.y, 5.0f, bounds.height},
                         0.5f, 6, accent);
    drawUiText(label, (int)bounds.x + 18, (int)bounds.y + 14, 14, COLOR_MUTED);
    drawDisplayText(value, (int)bounds.x + 18, (int)bounds.y + 42, 25, COLOR_TEXT);
}

/** Draw a rounded high-contrast action button and report a completed click. */
static bool brightButton(Rectangle bounds, const char *label, bool enabled) {
    const bool hovered = enabled && CheckCollisionPointRec(GetMousePosition(), bounds);
    const bool pressed = hovered && IsMouseButtonDown(MOUSE_BUTTON_LEFT);
    const Color fill = !enabled ? (Color){39, 49, 67, 255}
        : pressed ? (Color){34, 197, 181, 255}
        : hovered ? (Color){103, 232, 249, 255}
        : (Color){45, 212, 191, 255};
    DrawRectangleRounded(bounds, 0.34f, 12, fill);
    if (enabled) {
        DrawRectangleRoundedLinesEx(bounds, 0.34f, 12, 1.0f,
                                    hovered ? (Color){207, 250, 254, 255}
                                            : (Color){94, 234, 212, 255});
    }
    const int fontSize = 16;
    const Vector2 measured = MeasureTextEx(uiFont, label, (float)fontSize, 0.0f);
    DrawTextEx(uiFont, label,
               (Vector2){bounds.x + (bounds.width - measured.x) * 0.5f,
                         bounds.y + (bounds.height - measured.y) * 0.5f - 1.0f},
               (float)fontSize, 0.0f,
               enabled ? (Color){7, 22, 28, 255} : (Color){94, 107, 127, 255});
    return enabled && hovered && IsMouseButtonReleased(MOUSE_BUTTON_LEFT);
}

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
    /** Whether post-verification RX-mode synchronization has been requested. */
    bool modeQuerySent;
} ConnectionHandshake_t;

/** Objects used by the frame callback to update UI and handshake state. */
typedef struct {
    /** Rolling UI traffic log. */
    TrafficLog_t *log;
    /** Active connection verification state. */
    ConnectionHandshake_t *handshake;
} ClientFrameContext_t;

/** One selectable large deterministic-transfer size. */
typedef struct {
    /** Label shared with the Python client. */
    const char *label;
    /** Logical payload size in bytes. */
    uint32_t size;
} BulkSizePreset_t;

static const Preset_t presets[] = {
    {"HCI Reset",          0x0C03U, {0U},            0U},
    {"Read Local Version", 0x1001U, {0U},            0U},
    {"Read BD_ADDR",       0x1009U, {0U},            0U},
    {"LE Read Buffer Size",0x2002U, {0U},            0U},
    {"LE Enable Scan",     0x200CU, {0x01U, 0x00U}, 2U},
    {"LE Disable Scan",    0x200CU, {0x00U, 0x00U}, 2U},
};

/** Large-transfer choices mirrored by the Python UI. */
static const BulkSizePreset_t bulkSizePresets[] = {
    {"16 KiB",  16U * 1024U},
    {"64 KiB",  64U * 1024U},
    {"128 KiB", 128U * 1024U},
    {"512 KiB", 512U * 1024U},
    {"1 MiB",   1U * 1024U * 1024U},
    {"2 MiB",   2U * 1024U * 1024U},
    {"4 MiB",   4U * 1024U * 1024U},
    {"8 MiB",   8U * 1024U * 1024U},
    {"16 MiB", 16U * 1024U * 1024U},
};

/** Semicolon-separated labels consumed by raygui's dropdown control. */
static const char *const bulkSizeOptions[] = {
    "16 KiB", "64 KiB", "128 KiB", "512 KiB", "1 MiB",
    "2 MiB", "4 MiB", "8 MiB", "16 MiB",
};

/** Return whether the platform's standard clipboard modifier is held. */
static bool clipboardModifierDown(void) {
#if defined(__APPLE__)
    return IsKeyDown(KEY_LEFT_SUPER) || IsKeyDown(KEY_RIGHT_SUPER);
#else
    return IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
#endif
}

/** Replace a single-line field with printable clipboard text. */
static void pasteClipboardText(char *text, size_t capacity) {
    const char *const clipboard = GetClipboardText();
    if (clipboard == NULL || capacity == 0U) {
        return;
    }
    size_t output = 0U;
    for (size_t input = 0U; clipboard[input] != '\0' && output + 1U < capacity;
         input++) {
        const unsigned char value = (unsigned char)clipboard[input];
        if (value >= 0x20U && value != 0x7FU) {
            text[output++] = (char)value;
        }
    }
    text[output] = '\0';
}

/** Draw a raygui text box with portable whole-field clipboard shortcuts.
 *
 * Cmd is used on macOS and Ctrl on Windows and Linux. C copies the complete
 * active field, X copies and clears it, and V replaces it with clipboard text.
 */
static void drawClipboardTextBox(Rectangle bounds, char *text, size_t capacity,
                                 bool *editing) {
    const bool modifier = *editing && clipboardModifierDown();
    const bool copy = modifier && IsKeyPressed(KEY_C);
    const bool cut = modifier && IsKeyPressed(KEY_X);
    const bool paste = modifier && IsKeyPressed(KEY_V);
    const bool shortcut = copy || cut || paste;

    if (copy || cut) {
        SetClipboardText(text);
    }
    if (cut) {
        text[0] = '\0';
    } else if (paste) {
        pasteClipboardText(text, capacity);
    }

    /* Suppress normal character insertion on the shortcut frame. */
    if (GuiTextBox(bounds, text, (int)capacity, shortcut ? false : *editing)) {
        *editing = !*editing;
    }
}

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

/** Send a typed RX-mode set or query command. */
static bool sendRxModeRequest(AppSerialPort_t *port, TrafficLog_t *log,
                              uint8_t mode) {
    BleHciCommandPacket_t command;
    const size_t length = AppH4RxModeCommand_Init(&command, mode);
    return sendFramed(port, log, &command, length);
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
    handshake->modeQuerySent = false;
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
    log->receivedPackets++;
    const size_t h4Length = length - 2U;
    BleHciPacketIndicator_t indicator;
    memcpy(&indicator, frame, sizeof(indicator));
    bool byteIrqMode = false;
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
    if (testFrame && log->testIrqActive && log->receiveByteIrqActive) {
        log->testIrqCalls += (uint32_t)length;
    }
    if (testFrame && log->testIrqActive &&
        (testFlags & TEST_TRANSFER_FLAG_END) != 0U) {
        log->transmissionIrqCount = log->testIrqCalls;
        log->testIrqActive = false;
        irqCompletion = true;
    } else if (!testFrame) {
        log->transmissionIrqCount = log->receiveByteIrqActive
            ? (uint32_t)length : 0U;
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
    } else if (crcValid && AppH4RxModeEvent_Decode(frame, length, &byteIrqMode)) {
        log->byteIrqMode = byteIrqMode;
        log->receiveByteIrqActive = byteIrqMode;
        appendLog(log, "RX  Mode synchronized: %s",
                  byteIrqMode ? "per-byte IRQ" : "DMA + IDLE");
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
    for (unsigned int packet = 0U; packet < 64U && sender->offset < sender->totalSize; packet++) {
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
    const int windowWidth = 1120;
    const int windowHeight = 960;
    char windowTitle[96];
    (void)snprintf(windowTitle, sizeof(windowTitle), "BLE UART Client v%s",
                   UART_DMA_APP_VERSION);
    InitWindow(windowWidth, windowHeight, windowTitle);
    SetWindowMinSize(980, 820);
    SetTargetFPS(60);
    loadUiFont();

    AppSerialPort_t serial = {0};
    AppSerialPort_Init(&serial);
    AppH4StreamParser_t parser;
    AppH4StreamParser_Init(&parser);
    TrafficLog_t log = {{{0}}, 0U, 0U, 0U, false, false,
                        0U, 0U, 0U, false, 0U};
    TestSender_t testSender = {0U, 0U, 0U, 0U, false};
    ConnectionHandshake_t handshake = {0};
    ClientFrameContext_t frameContext = {&log, &handshake};
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
    bool bulkSizeDropdown = false;
    int presetIndex = 0;
    int previousPreset = 0;
    int bulkSizeIndex = 5;
    presetToFields(0, opcodeText, sizeof(opcodeText), parametersText, sizeof(parametersText));
    appendLog(&log, "Ready. Start ble_uart_server and enter its printed port.");

    while (!WindowShouldClose()) {
        if (serial.connected) {
            uint8_t incoming[16U * 1024U];
            const int received = AppSerialPort_Read(&serial, incoming, sizeof(incoming));
            if (received > 0) {
                if (log.receiveByteIrqActive) {
                    log.absoluteIrqCount += (uint32_t)received;
                }
                (void)AppH4StreamParser_Feed(
                    &parser, incoming, (size_t)received, logFrame, &frameContext);
            } else if (received < 0) {
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
                   !handshake.modeQuerySent) {
            handshake.modeQuerySent = true;
            (void)snprintf(statusText, sizeof(statusText), "Connected to %s", portText);
            appendLog(&log, "Connected to %s; server identity verified", portText);
            (void)sendRxModeRequest(&serial, &log, BLE_HCI_RX_MODE_QUERY);
        }
        pumpTestTransfer(&testSender, &serial, &log);

        /* Raygui text boxes have caller-owned focus; keep exactly one active. */
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            portEdit = false;
            baudEdit = false;
            opcodeEdit = false;
            parametersEdit = false;
            rawEdit = false;
        }

        BeginDrawing();
        ClearBackground(COLOR_CANVAS);
        char heading[96];
        (void)snprintf(heading, sizeof(heading), "BLE UART HCI Client  v%s",
                       UART_DMA_APP_VERSION);
        drawDisplayText(heading, 28, 22, 32, COLOR_TEXT);
        drawUiText("PORTABLE H4 CONSOLE  /  CRC16-CCITT  /  LIVE PACKET TELEMETRY",
                   30, 62, 14, COLOR_MUTED);

        char txMetric[32];
        char rxMetric[32];
        char irqMetric[32];
        (void)snprintf(txMetric, sizeof(txMetric), "%u",
                       (unsigned int)log.transmittedPackets);
        (void)snprintf(rxMetric, sizeof(rxMetric), "%u",
                       (unsigned int)log.receivedPackets);
        (void)snprintf(irqMetric, sizeof(irqMetric), "%u",
                       (unsigned int)log.absoluteIrqCount);
        drawMetricCard((Rectangle){24, 100, 259, 84}, "RX PACKETS", rxMetric, COLOR_BLUE);
        drawMetricCard((Rectangle){295, 100, 259, 84}, "TX PACKETS", txMetric, COLOR_CYAN);
        drawMetricCard((Rectangle){566, 100, 259, 84}, "RX IRQ CALLS", irqMetric, COLOR_PURPLE);
        drawMetricCard((Rectangle){837, 100, 259, 84}, "TRANSPORT",
                       handshake.pending ? "VERIFYING" : serial.connected
                           ? (log.byteIrqMode ? "BYTE IRQ" : "DMA + IDLE")
                           : "OFFLINE",
                       serial.connected ? COLOR_CYAN : COLOR_MUTED);

        drawPanel((Rectangle){24, 204, 1072, 136}, "CONNECTION");
        GuiLabel((Rectangle){44, 247, 50, 38}, "Port");
        drawClipboardTextBox((Rectangle){96, 247, 426, 38}, portText,
                             sizeof(portText), &portEdit);
        GuiLabel((Rectangle){542, 247, 52, 38}, "Baud");
        drawClipboardTextBox((Rectangle){598, 247, 138, 38}, baudText,
                             sizeof(baudText), &baudEdit);
        if (brightButton((Rectangle){756, 247, 170, 38},
                         serial.connected ? "Disconnect" : "Connect", true)) {
            if (serial.connected) {
                AppSerialPort_Close(&serial);
                handshake.pending = false;
                handshake.verified = false;
                (void)snprintf(statusText, sizeof(statusText), "Disconnected");
                appendLog(&log, "Disconnected from %s", portText);
            } else {
                char *end = NULL;
                const unsigned long baud = strtoul(baudText, &end, 10);
                char error[180];
                if (end == baudText || *end != '\0') {
                    (void)snprintf(statusText, sizeof(statusText), "Invalid baud rate");
                } else if (AppSerialPort_Open(&serial, portText, baud, error, sizeof(error))) {
                    AppH4StreamParser_Init(&parser);
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
        GuiStatusBar((Rectangle){44, 298, 882, 28}, statusText);
        drawUiText("Native clipboard shortcuts enabled", 46, 329, 11, COLOR_MUTED);
        const bool previousRxMode = log.byteIrqMode;
        const bool connectionReady = serial.connected && handshake.verified;
        if (!connectionReady) {
            GuiDisable();
        }
        const bool rxModeChanged = GuiCheckBox((Rectangle){958, 254, 24, 24},
                                               "Per-byte IRQ", &log.byteIrqMode) != 0;
        if (!connectionReady) {
            GuiEnable();
        }
        if (rxModeChanged && !sendRxModeRequest(
                &serial, &log, log.byteIrqMode
                    ? BLE_HCI_RX_MODE_BYTE_IRQ : BLE_HCI_RX_MODE_DMA)) {
            log.byteIrqMode = previousRxMode;
        }

        drawPanel((Rectangle){24, 360, 1072, 216}, "HCI COMMAND BUILDER");
        GuiLabel((Rectangle){44, 397, 64, 38}, "Preset");
        if (GuiDropdownBox((Rectangle){112, 397, 360, 38},
                           "HCI Reset;Read Local Version;Read BD_ADDR;LE Read Buffer Size;LE Enable Scan;LE Disable Scan",
                           &presetIndex, presetDropdown)) {
            presetDropdown = !presetDropdown;
        }
        if (presetIndex != previousPreset) {
            presetToFields(presetIndex, opcodeText, sizeof(opcodeText),
                           parametersText, sizeof(parametersText));
            previousPreset = presetIndex;
        }
        GuiLabel((Rectangle){496, 397, 66, 38}, "Opcode");
        drawClipboardTextBox((Rectangle){566, 397, 164, 38}, opcodeText,
                             sizeof(opcodeText), &opcodeEdit);
        GuiLabel((Rectangle){44, 452, 132, 38}, "Parameters");
        drawClipboardTextBox((Rectangle){180, 452, 646, 38}, parametersText,
                             sizeof(parametersText), &parametersEdit);
        if (brightButton((Rectangle){850, 452, 226, 38}, "Send command",
                         connectionReady)) {
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
        drawClipboardTextBox((Rectangle){180, 512, 646, 38}, rawText,
                             sizeof(rawText), &rawEdit);
        if (brightButton((Rectangle){850, 512, 226, 38}, "Append CRC + send",
                         connectionReady)) {
            uint8_t raw[APP_H4_MAX_FRAME_SIZE - 2U];
            const size_t rawLength = AppH4_ParseHex(rawText, raw, sizeof(raw));
            if (rawLength == SIZE_MAX || rawLength == 0U) {
                appendLog(&log, "ERROR  Enter valid raw H4 hexadecimal bytes");
            } else {
                (void)sendFramed(&serial, &log, raw, rawLength);
            }
        }

        drawPanel((Rectangle){24, 596, 1072, 124}, "TEST TRANSMISSIONS");
        if (brightButton((Rectangle){44, 638, 220, 42}, "Send 128-byte packet",
                         connectionReady)) {
            startTestTransfer(&testSender, &serial, &log, 128U);
        }
        GuiLabel((Rectangle){292, 640, 92, 38}, "Block size");
        if (brightButton((Rectangle){548, 638, 218, 42}, "Send selected block",
                         connectionReady && !testSender.active)) {
            startTestTransfer(&testSender, &serial, &log,
                              bulkSizePresets[bulkSizeIndex].size);
        }
        const float progress = testSender.totalSize == 0U ? 0.0f
            : (float)testSender.offset / (float)testSender.totalSize;
        DrawRectangleRounded((Rectangle){792, 638, 284, 16}, 0.5f, 6,
                             (Color){35, 45, 65, 255});
        DrawRectangleRounded((Rectangle){792, 638, 284.0f * progress, 16},
                             0.5f, 6, COLOR_BLUE);
        char progressText[96];
        (void)snprintf(progressText, sizeof(progressText), "%u / %u bytes",
                       (unsigned int)testSender.offset,
                       (unsigned int)testSender.totalSize);
        drawUiText(progressText, 792, 665, 14, COLOR_MUTED);

        drawPanel((Rectangle){24, 740, 1072, 196}, "LIVE UART TRAFFIC");
        DrawRectangleRounded((Rectangle){44, 777, 1032, 142}, 0.04f, 6,
                             (Color){7, 11, 20, 255});
        for (size_t index = 0U; index < log.count; index++) {
            const Color color = strncmp(log.lines[index], "ERROR", 5U) == 0
                ? (Color){255, 120, 120, 255}
                : strncmp(log.lines[index], "TX", 2U) == 0
                    ? (Color){100, 190, 255, 255}
                    : strncmp(log.lines[index], "RX", 2U) == 0
                        ? (Color){105, 225, 150, 255}
                        : (Color){190, 198, 210, 255};
            drawUiText(log.lines[index], 58, 786 + (int)index * 15, 13, color);
        }

        /* Draw last so the expanded selector overlays the traffic panel. */
        (void)AppDashboard_UpwardDropdown(
            (Rectangle){392, 638, 132, 42}, uiFont, bulkSizeOptions,
            sizeof(bulkSizeOptions) / sizeof(bulkSizeOptions[0]),
            &bulkSizeIndex, &bulkSizeDropdown);
        EndDrawing();
    }

    AppSerialPort_Close(&serial);
    if (customUiFont) {
        UnloadFont(uiFont);
    }
    if (customDisplayFont) {
        UnloadFont(displayFont);
    }
    CloseWindow();
    return 0;
}
