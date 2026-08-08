/** @file
 * @brief Portable graphical supervisor for the continuous BLE UART server. */
#include "raylib.h"
#include "h4_wire.h"
#include "test_transfer_protocol.h"
#include "uart_dma/app_version.h"
#include "uart_dma/uart_dma_messages.h"

#define RAYGUI_IMPLEMENTATION
#include "raygui.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_LOG_LINES       11U
#define MAX_LOG_LENGTH      220U
#define PROCESS_BUFFER_SIZE 2048U

static Font uiFont;
static bool customUiFont;

/** Load the bundled sans-serif UI font and configure raygui to use it. */
static void loadUiFont(void) {
    const char *path = TextFormat("%s%s", GetApplicationDirectory(),
                                  UART_DMA_UI_FONT_FILE);
    uiFont = LoadFontEx(path, 28, NULL, 0);
    customUiFont = uiFont.texture.id != 0U;
    if (!customUiFont) {
        uiFont = GetFontDefault();
    }
    SetTextureFilter(uiFont.texture, TEXTURE_FILTER_BILINEAR);
    GuiSetFont(uiFont);
    GuiSetStyle(DEFAULT, TEXT_SIZE, 15);
}

/** Draw antialiased text using the shared UI typeface. */
static void drawUiText(const char *text, int x, int y, int size, Color color) {
    DrawTextEx(uiFont, text, (Vector2){(float)x, (float)y}, (float)size, 0.0f, color);
}

/** HCI command preset mirrored from the client applications. */
typedef struct {
    /** Human-readable picker label. */
    const char *name;
    /** Command opcode in host byte order. */
    uint16_t opcode;
    /** Fixed preset parameters. */
    uint8_t parameters[8];
    /** Number of valid parameter bytes. */
    size_t parameterLength;
} Preset_t;

/** One shared deterministic-transfer size. */
typedef struct {
    /** Dropdown label. */
    const char *label;
    /** Logical payload bytes. */
    uint32_t size;
} BulkSizePreset_t;

static const Preset_t presets[] = {
    {"HCI Reset",           0x0C03U, {0U},             0U},
    {"Read Local Version",  0x1001U, {0U},             0U},
    {"Read BD_ADDR",        0x1009U, {0U},             0U},
    {"LE Read Buffer Size", 0x2002U, {0U},             0U},
    {"LE Enable Scan",      0x200CU, {0x01U, 0x00U},  2U},
    {"LE Disable Scan",     0x200CU, {0x00U, 0x00U},  2U},
};

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

static const char bulkSizeOptions[] =
    "16 KiB;64 KiB;128 KiB;512 KiB;1 MiB;2 MiB;4 MiB;8 MiB;16 MiB";
static const char presetOptions[] =
    "HCI Reset;Read Local Version;Read BD_ADDR;LE Read Buffer Size;LE Enable Scan;LE Disable Scan";

/** Fixed-height rolling server log. */
typedef struct {
    /** Render-ready lines. */
    char lines[MAX_LOG_LINES][MAX_LOG_LENGTH];
    /** Number of populated lines. */
    size_t count;
} ServerLog_t;

/** Child-process and partial-line state for the server engine. */
typedef struct {
    /** Server child PID. */
    pid_t processId;
    /** Nonblocking descriptor receiving combined stdout and stderr. */
    int outputDescriptor;
    /** Descriptor sending mode-control lines to the child process. */
    int inputDescriptor;
    /** Whether the child has not yet been reaped. */
    bool running;
    /** Whether termination has already been requested. */
    bool stopping;
    /** Incomplete trailing output line. */
    char partialLine[PROCESS_BUFFER_SIZE];
    /** Valid bytes in `partialLine`. */
    size_t partialLength;
} ServerProcess_t;

/** Metrics extracted from textual server output. */
typedef struct {
    /** Current client-facing pseudo-UART device path. */
    char port[128];
    /** Count of command or test-message starts. */
    unsigned int receivedCommands;
    /** Count of transmitted event log lines. */
    unsigned int transmittedEvents;
    /** Most recent child exit status. */
    int exitStatus;
    /** Authoritative mode parsed from the server engine log. */
    bool byteIrqMode;
    /** Actual number of RXNE interrupts serviced by the UART ISR. */
    uint32_t rxInterruptCount;
    /** Logical bytes sent by a dashboard-originated test transfer. */
    uint32_t testSent;
    /** Requested dashboard-originated test-transfer size. */
    uint32_t testTotal;
    /** Whether the server is currently generating test chunks. */
    bool testActive;
} ServerState_t;

/** Return whether the platform clipboard modifier is currently held. */
static bool clipboardModifierDown(void) {
#if defined(__APPLE__)
    return IsKeyDown(KEY_LEFT_SUPER) || IsKeyDown(KEY_RIGHT_SUPER);
#else
    return IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
#endif
}

/** Replace a field with printable clipboard contents. */
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

/** Draw a whole-field clipboard-aware raygui text box. */
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
    if (GuiTextBox(bounds, text, (int)capacity, shortcut ? false : *editing)) {
        *editing = !*editing;
    }
}

/** Append one server log line, evicting the oldest line when full. */
static void appendLog(ServerLog_t *log, const char *line) {
    if (log->count == MAX_LOG_LINES) {
        memmove(log->lines, &log->lines[1],
                (MAX_LOG_LINES - 1U) * sizeof(log->lines[0]));
        log->count--;
    }
    (void)snprintf(log->lines[log->count], MAX_LOG_LENGTH, "%s", line);
    log->count++;
}

/** Update metrics and connection state from one complete engine log line. */
static void handleServerLine(ServerLog_t *log, ServerState_t *state,
                             const char *line) {
    static const char portMarker[] = "type this port manually: ";
    const char *port = strstr(line, portMarker);
    if (port != NULL) {
        port += sizeof(portMarker) - 1U;
        (void)snprintf(state->port, sizeof(state->port), "%s", port);
    }
    if (strcmp(line, "RX MODE -> BYTE IRQ") == 0) {
        state->byteIrqMode = true;
    } else if (strcmp(line, "RX MODE -> DMA") == 0) {
        state->byteIrqMode = false;
    }
    unsigned int parsed = 0U;
    unsigned int total = 0U;
    unsigned int transmissionIrqs = 0U;
    if (sscanf(line, "RX IRQ COUNT -> %u", &parsed) == 1) {
        state->rxInterruptCount = parsed;
        return;
    } else if (sscanf(line,
                      "RX IRQ TRANSMISSION -> absolute=%u transmission=%u",
                      &parsed, &transmissionIrqs) == 2) {
        state->rxInterruptCount = parsed;
    } else if (sscanf(line, "TX DMA <- TEST message start id=%*u size=%u bytes",
                      &total) == 1) {
        state->testSent = 0U;
        state->testTotal = total;
        state->testActive = true;
    } else if (sscanf(line, "TX DMA <- TEST progress %u/%u bytes", &parsed, &total) == 2) {
        state->testSent = parsed;
        state->testTotal = total;
    } else if (sscanf(line, "TX DMA <- TEST message complete id=%*u size=%u bytes",
                      &parsed) == 1) {
        state->testSent = parsed;
        state->testTotal = parsed;
        state->testActive = false;
    } else if (strstr(line, "TX DMA <- TEST message failed") != NULL) {
        state->testActive = false;
    }
    if (strncmp(line, "RX DMA -> HCI command", 21U) == 0 ||
        strncmp(line, "RX DMA -> TEST message start", 28U) == 0) {
        state->receivedCommands++;
    } else if (strncmp(line, "TX DMA", 6U) == 0) {
        state->transmittedEvents++;
    }
    appendLog(log, line);
}

/** Split arbitrary child-process output chunks into complete lines. */
static void consumeProcessOutput(ServerProcess_t *process, ServerLog_t *log,
                                 ServerState_t *state, const char *data,
                                 size_t length) {
    for (size_t index = 0U; index < length; index++) {
        const char value = data[index];
        if (value == '\n') {
            process->partialLine[process->partialLength] = '\0';
            if (process->partialLength != 0U) {
                handleServerLine(log, state, process->partialLine);
            }
            process->partialLength = 0U;
        } else if (value != '\r') {
            if (process->partialLength + 1U < sizeof(process->partialLine)) {
                process->partialLine[process->partialLength++] = value;
            }
        }
    }
}

/** Fork and connect the bundled continuous server engine. */
static bool startServer(ServerProcess_t *process, ServerLog_t *log,
                        ServerState_t *state) {
    int outputPipe[2] = {-1, -1};
    int inputPipe[2] = {-1, -1};
    if (pipe(outputPipe) != 0 || pipe(inputPipe) != 0) {
        if (outputPipe[0] >= 0) {
            close(outputPipe[0]);
            close(outputPipe[1]);
        }
        appendLog(log, "ERROR  Unable to create server output pipe");
        return false;
    }

    char helperPath[1024];
#if defined(__APPLE__)
    (void)snprintf(helperPath, sizeof(helperPath), "%s/ble_uart_server_engine",
                   GetApplicationDirectory());
#else
    (void)snprintf(helperPath, sizeof(helperPath), "%s/ble_uart_server",
                   GetApplicationDirectory());
#endif

    const pid_t child = fork();
    if (child < 0) {
        close(outputPipe[0]);
        close(outputPipe[1]);
        close(inputPipe[0]);
        close(inputPipe[1]);
        appendLog(log, "ERROR  Unable to start BLE server process");
        return false;
    }
    if (child == 0) {
        close(outputPipe[0]);
        close(inputPipe[1]);
        (void)dup2(outputPipe[1], STDOUT_FILENO);
        (void)dup2(outputPipe[1], STDERR_FILENO);
        (void)dup2(inputPipe[0], STDIN_FILENO);
        close(outputPipe[1]);
        close(inputPipe[0]);
        execl(helperPath, helperPath, (char *)NULL);
        perror("Unable to launch bundled BLE server engine");
        _exit(127);
    }

    close(outputPipe[1]);
    close(inputPipe[0]);
    (void)fcntl(outputPipe[0], F_SETFL, fcntl(outputPipe[0], F_GETFL) | O_NONBLOCK);
    process->processId = child;
    process->outputDescriptor = outputPipe[0];
    process->inputDescriptor = inputPipe[1];
    process->running = true;
    process->stopping = false;
    process->partialLength = 0U;
    state->port[0] = '\0';
    state->receivedCommands = 0U;
    state->transmittedEvents = 0U;
    state->exitStatus = 0;
    state->byteIrqMode = false;
    state->rxInterruptCount = 0U;
    state->testSent = 0U;
    state->testTotal = 0U;
    state->testActive = false;
    appendLog(log, "Starting continuous FreeRTOS BLE server engine...");
    return true;
}

/** Request graceful termination of a running server engine. */
static void requestServerStop(ServerProcess_t *process, ServerLog_t *log) {
    if (process->running && !process->stopping) {
        process->stopping = true;
        (void)kill(process->processId, SIGTERM);
        appendLog(log, "Stopping BLE server engine...");
    }
}

/** Drain engine output and reap the child after it exits. */
static void pollServer(ServerProcess_t *process, ServerLog_t *log,
                       ServerState_t *state) {
    if (!process->running) {
        return;
    }

    char data[512];
    for (;;) {
        const ssize_t count = read(process->outputDescriptor, data, sizeof(data));
        if (count > 0) {
            consumeProcessOutput(process, log, state, data, (size_t)count);
        } else {
            break;
        }
    }

    int status = 0;
    const pid_t result = waitpid(process->processId, &status, WNOHANG);
    if (result == process->processId) {
        if (process->partialLength != 0U) {
            process->partialLine[process->partialLength] = '\0';
            handleServerLine(log, state, process->partialLine);
        }
        close(process->outputDescriptor);
        close(process->inputDescriptor);
        process->running = false;
        process->stopping = false;
        state->exitStatus = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        appendLog(log, state->exitStatus == 0
            ? "BLE server stopped cleanly"
            : "ERROR  BLE server engine exited unexpectedly");
    }
}

/** Write one complete dashboard control line to the server engine. */
static bool sendServerCommand(ServerProcess_t *process, const char *command) {
    if (!process->running) {
        return false;
    }
    size_t offset = 0U;
    const size_t length = strlen(command);
    while (offset < length) {
        const ssize_t written = write(process->inputDescriptor, &command[offset], length - offset);
        if (written <= 0) {
            return false;
        }
        offset += (size_t)written;
    }
    return true;
}

/** Send a receive-mode request to the running server engine. */
static bool sendServerMode(ServerProcess_t *process, bool byteIrqMode) {
    return sendServerCommand(process, byteIrqMode ? "irq\n" : "dma\n");
}

/** Serialize one H4 packet into a dashboard-to-engine control line. */
static bool sendServerPacket(ServerProcess_t *process, ServerLog_t *log,
                             const void *packet, size_t length) {
    if (packet == NULL || length == 0U ||
        length > UART_DMA_MAX_PACKET_SIZE - sizeof(BleHciCrcTrailer_t)) {
        appendLog(log, "ERROR  Server TX packet exceeds the DMA queue capacity");
        return false;
    }
    char hexadecimal[UART_DMA_MAX_PACKET_SIZE * 3U];
    AppH4_FormatHex(hexadecimal, sizeof(hexadecimal), packet, length);
    char command[sizeof(hexadecimal) + 5U];
    (void)snprintf(command, sizeof(command), "tx %s\n", hexadecimal);
    if (!sendServerCommand(process, command)) {
        appendLog(log, "ERROR  Unable to send packet request to server engine");
        return false;
    }
    return true;
}

/** Construct and queue a typed HCI command from the server dashboard. */
static bool sendServerHciCommand(ServerProcess_t *process, ServerLog_t *log,
                                 uint16_t opcode, const uint8_t *parameters,
                                 size_t parameterLength) {
    BleHciCommandPacket_t command;
    const size_t length = AppH4Command_Init(
        &command, opcode, parameters, parameterLength);
    return length != 0U && sendServerPacket(process, log, &command, length);
}

/** Copy a selected preset into the editable HCI fields. */
static void presetToFields(int presetIndex, char *opcode, size_t opcodeSize,
                           char *parameters, size_t parameterSize) {
    const Preset_t *const preset = &presets[presetIndex];
    (void)snprintf(opcode, opcodeSize, "%04X", preset->opcode);
    AppH4_FormatHex(parameters, parameterSize,
                    preset->parameters, preset->parameterLength);
}

/** Request one deterministic test transfer from the server engine. */
static bool startServerTest(ServerProcess_t *process, ServerLog_t *log,
                            ServerState_t *state, uint32_t totalSize) {
    if (state->testActive) {
        appendLog(log, "ERROR  A server test transfer is already active");
        return false;
    }
    char command[48];
    (void)snprintf(command, sizeof(command), "test %u\n", (unsigned int)totalSize);
    if (!sendServerCommand(process, command)) {
        appendLog(log, "ERROR  Unable to start server test transfer");
        return false;
    }
    state->testSent = 0U;
    state->testTotal = totalSize;
    state->testActive = true;
    return true;
}

/** Render one dashboard metric card. */
static void drawMetricCard(Rectangle bounds, const char *label,
                           const char *value, Color accent) {
    DrawRectangleRounded(bounds, 0.10f, 8, WHITE);
    DrawRectangleRounded((Rectangle){bounds.x, bounds.y, 5.0f, bounds.height},
                         0.5f, 4, accent);
    drawUiText(label, (int)bounds.x + 18, (int)bounds.y + 14, 14,
               (Color){100, 110, 128, 255});
    drawUiText(value, (int)bounds.x + 18, (int)bounds.y + 40, 24,
               (Color){25, 35, 55, 255});
}

/** Run the graphical server dashboard and child-process supervisor. */
int main(void) {
    char windowTitle[96];
    (void)snprintf(windowTitle, sizeof(windowTitle), "BLE UART Server v%s",
                   UART_DMA_APP_VERSION);
    InitWindow(960, 940, windowTitle);
    SetWindowMinSize(760, 700);
    SetTargetFPS(60);
    loadUiFont();

    ServerProcess_t process = {0, -1, -1, false, false, {0}, 0U};
    ServerState_t state = {{0}, 0U, 0U, 0, false, 0U, 0U, 0U, false};
    ServerLog_t log = {{{0}}, 0U};
    char opcodeText[16] = "0C03";
    char parametersText[768] = "";
    char rawText[1024] = "04 0E 04 01 03 0C 00";
    bool opcodeEdit = false;
    bool parametersEdit = false;
    bool rawEdit = false;
    bool presetDropdown = false;
    bool bulkSizeDropdown = false;
    int presetIndex = 0;
    int previousPreset = 0;
    int bulkSizeIndex = 5;
    unsigned int statsFrame = 0U;
    presetToFields(0, opcodeText, sizeof(opcodeText),
                   parametersText, sizeof(parametersText));
    appendLog(&log, "Ready. Start the server to create a local pseudo-UART.");

    while (!WindowShouldClose()) {
        pollServer(&process, &log, &state);
        if (process.running && ++statsFrame >= 30U) {
            (void)sendServerCommand(&process, "stats\n");
            statsFrame = 0U;
        }

        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            opcodeEdit = false;
            parametersEdit = false;
            rawEdit = false;
        }

        BeginDrawing();
        ClearBackground((Color){245, 247, 250, 255});
        char heading[96];
        (void)snprintf(heading, sizeof(heading), "BLE UART DMA Server  v%s",
                       UART_DMA_APP_VERSION);
        drawUiText(heading, 24, 20, 26, (Color){25, 35, 55, 255});
        drawUiText("FreeRTOS | circular RX DMA | HCI command responder",
                   24, 52, 16, (Color){90, 100, 120, 255});

        const Color serverColor = process.running
            ? (process.stopping ? (Color){240, 170, 60, 255} : (Color){45, 185, 105, 255})
            : (Color){145, 152, 166, 255};
        DrawCircle(35, 110, 7.0f, serverColor);
        drawUiText(process.running ? (process.stopping ? "Stopping" : "Server running")
                                   : "Server stopped",
                   51, 100, 20, (Color){40, 48, 62, 255});

        if (GuiButton((Rectangle){724, 91, 212, 38},
                      process.running ? "Stop server" : "Start server")) {
            if (process.running) {
                requestServerStop(&process, &log);
            } else {
                (void)startServer(&process, &log, &state);
            }
        }

        char rxValue[32];
        char txValue[32];
        char irqValue[32];
        (void)snprintf(rxValue, sizeof(rxValue), "%u", state.receivedCommands);
        (void)snprintf(txValue, sizeof(txValue), "%u", state.transmittedEvents);
        (void)snprintf(irqValue, sizeof(irqValue), "%u",
                       (unsigned int)state.rxInterruptCount);
        drawMetricCard((Rectangle){24, 151, 218, 82}, "RX messages", rxValue,
                       (Color){70, 145, 235, 255});
        drawMetricCard((Rectangle){258, 151, 218, 82}, "TX activity", txValue,
                       (Color){45, 185, 105, 255});
        drawMetricCard((Rectangle){492, 151, 218, 82}, "RX IRQ calls", irqValue,
                       (Color){170, 105, 225, 255});
        drawMetricCard((Rectangle){726, 151, 210, 82}, "Transport",
                       process.running
                           ? (state.byteIrqMode ? "Byte IRQ active" : "DMA active")
                           : "Idle",
                       serverColor);

        GuiGroupBox((Rectangle){20, 250, 920, 96}, "Client connection");
        GuiLabel((Rectangle){36, 271, 125, 24}, "Pseudo-UART port");
        GuiStatusBar((Rectangle){163, 268, 568, 30},
                     state.port[0] != '\0' ? state.port : "Start the server to create a port");
        if (GuiButton((Rectangle){748, 268, 168, 30}, "Copy port")) {
            if (state.port[0] != '\0') {
                SetClipboardText(state.port);
                appendLog(&log, "Copied pseudo-UART port to clipboard");
            }
        }
        GuiLabel((Rectangle){36, 307, 650, 24},
                 "Paste this port into the Python app, C terminal client, or C client UI.");
        const bool previousMode = state.byteIrqMode;
        if (!process.running) {
            GuiDisable();
        }
        const bool modeChanged = GuiCheckBox((Rectangle){748, 307, 20, 20},
                                             "Per-byte IRQ", &state.byteIrqMode) != 0;
        if (!process.running) {
            GuiEnable();
        }
        if (modeChanged && !sendServerMode(&process, state.byteIrqMode)) {
            state.byteIrqMode = previousMode;
            appendLog(&log, "ERROR  Unable to send receive-mode request");
        }

        GuiGroupBox((Rectangle){20, 363, 920, 176}, "Send HCI packet to client");
        GuiLabel((Rectangle){36, 383, 65, 24}, "Preset");
        if (GuiDropdownBox((Rectangle){103, 380, 280, 30}, presetOptions,
                           &presetIndex, presetDropdown)) {
            presetDropdown = !presetDropdown;
        }
        if (presetIndex != previousPreset) {
            presetToFields(presetIndex, opcodeText, sizeof(opcodeText),
                           parametersText, sizeof(parametersText));
            previousPreset = presetIndex;
        }
        GuiLabel((Rectangle){408, 383, 60, 24}, "Opcode");
        drawClipboardTextBox((Rectangle){470, 380, 120, 30}, opcodeText,
                             sizeof(opcodeText), &opcodeEdit);
        GuiLabel((Rectangle){36, 428, 110, 24}, "Parameters (hex)");
        drawClipboardTextBox((Rectangle){148, 425, 598, 30}, parametersText,
                             sizeof(parametersText), &parametersEdit);
        if (!process.running) {
            GuiDisable();
        }
        const bool sendCommandPressed = GuiButton(
            (Rectangle){764, 425, 152, 30}, "Send command");
        if (!process.running) {
            GuiEnable();
        }
        if (sendCommandPressed) {
            char *end = NULL;
            const unsigned long opcode = strtoul(opcodeText, &end, 16);
            uint8_t parameters[BLE_HCI_MAX_COMMAND_PARAMETERS];
            const size_t parameterLength = AppH4_ParseHex(
                parametersText, parameters, sizeof(parameters));
            if (end == opcodeText || *end != '\0' || opcode > UINT16_MAX) {
                appendLog(&log, "ERROR  Invalid 16-bit hexadecimal opcode");
            } else if (parameterLength == SIZE_MAX) {
                appendLog(&log, "ERROR  Invalid hexadecimal parameter bytes");
            } else {
                (void)sendServerHciCommand(&process, &log, (uint16_t)opcode,
                                           parameters, parameterLength);
            }
        }
        GuiLabel((Rectangle){36, 478, 110, 24}, "Raw H4 (hex)");
        drawClipboardTextBox((Rectangle){148, 475, 598, 30}, rawText,
                             sizeof(rawText), &rawEdit);
        if (!process.running) {
            GuiDisable();
        }
        const bool sendRawPressed = GuiButton(
            (Rectangle){764, 475, 152, 30}, "Append CRC + send");
        if (!process.running) {
            GuiEnable();
        }
        if (sendRawPressed) {
            uint8_t raw[UART_DMA_MAX_PACKET_SIZE - sizeof(BleHciCrcTrailer_t)];
            const size_t rawLength = AppH4_ParseHex(rawText, raw, sizeof(raw));
            if (rawLength == SIZE_MAX || rawLength == 0U) {
                appendLog(&log, "ERROR  Enter valid raw H4 hexadecimal bytes");
            } else {
                (void)sendServerPacket(&process, &log, raw, rawLength);
            }
        }

        GuiGroupBox((Rectangle){20, 556, 920, 100}, "Send test messages to client");
        if (!process.running || state.testActive) {
            GuiDisable();
        }
        const bool sendSmallTest = GuiButton(
            (Rectangle){36, 580, 184, 34}, "Send 128-byte message");
        GuiLabel((Rectangle){234, 585, 76, 24}, "Large block");
        const bool sendLargeTest = GuiButton(
            (Rectangle){458, 580, 166, 34}, "Send selected block");
        if (!process.running || state.testActive) {
            GuiEnable();
        }
        if (sendSmallTest) {
            (void)startServerTest(&process, &log, &state, 128U);
        }
        if (sendLargeTest) {
            (void)startServerTest(&process, &log, &state,
                                  bulkSizePresets[bulkSizeIndex].size);
        }
        const float progress = state.testTotal == 0U ? 0.0f
            : (float)state.testSent / (float)state.testTotal;
        DrawRectangle(642, 586, 272, 20, (Color){220, 225, 232, 255});
        DrawRectangle(642, 586, (int)(272.0f * progress), 20,
                      (Color){70, 145, 235, 255});
        DrawRectangleLines(642, 586, 272, 20, (Color){150, 158, 172, 255});
        char progressText[96];
        (void)snprintf(progressText, sizeof(progressText), "%u / %u bytes",
                       (unsigned int)state.testSent,
                       (unsigned int)state.testTotal);
        drawUiText(progressText, 642, 618, 14, (Color){80, 90, 108, 255});

        GuiGroupBox((Rectangle){20, 673, 920, 247}, "Live DMA / HCI traffic");
        DrawRectangle(34, 697, 892, 207, (Color){20, 25, 35, 255});
        for (size_t index = 0U; index < log.count; index++) {
            const Color color = strncmp(log.lines[index], "ERROR", 5U) == 0
                ? (Color){255, 120, 120, 255}
                : strncmp(log.lines[index], "RX DMA", 6U) == 0
                    ? (Color){100, 190, 255, 255}
                    : strncmp(log.lines[index], "TX DMA", 6U) == 0
                        ? (Color){105, 225, 150, 255}
                        : (Color){190, 198, 210, 255};
            drawUiText(log.lines[index], 46, 709 + (int)index * 17, 13, color);
        }

        /* Draw dropdowns last so their expanded lists overlay lower panels. */
        if (GuiDropdownBox((Rectangle){310, 580, 132, 34}, bulkSizeOptions,
                           &bulkSizeIndex, bulkSizeDropdown)) {
            bulkSizeDropdown = !bulkSizeDropdown;
        }
        EndDrawing();
    }

    if (process.running) {
        requestServerStop(&process, &log);
        (void)waitpid(process.processId, NULL, 0);
        close(process.outputDescriptor);
        close(process.inputDescriptor);
    }
    if (customUiFont) {
        UnloadFont(uiFont);
    }
    CloseWindow();
    return 0;
}
