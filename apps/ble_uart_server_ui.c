/** @file
 * @brief Portable graphical supervisor for the continuous BLE UART server. */
#include "raylib.h"
#include "dashboard_font.h"
#include "dashboard_dropdown.h"
#include "dashboard_log.h"
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

#define PROCESS_BUFFER_SIZE 2048U

static Font uiFont;
static AppDashboardFontSet_t dashboardFonts;

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
    const char *displayPath = TextFormat("%s%s", GetApplicationDirectory(),
                                         UART_DMA_UI_DISPLAY_FONT_FILE);
    AppDashboardFont_Load(&dashboardFonts, path, displayPath);
    uiFont = AppDashboardFont_Regular(&dashboardFonts, 15);
    GuiSetFont(uiFont);
    GuiSetStyle(DEFAULT, TEXT_SIZE, AppDashboardFont_RenderSize(15));
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
    GuiSetStyle(TEXTBOX, BORDER_WIDTH, 0);
    GuiSetStyle(TEXTBOX, TEXT_ALIGNMENT, TEXT_ALIGN_LEFT);
    GuiSetStyle(TEXTBOX, TEXT_PADDING, 10);
    GuiSetStyle(TEXTBOX, BASE_COLOR_NORMAL, ColorToInt(BLANK));
    GuiSetStyle(TEXTBOX, BASE_COLOR_FOCUSED, ColorToInt(BLANK));
    GuiSetStyle(TEXTBOX, BASE_COLOR_PRESSED, ColorToInt(BLANK));
    GuiSetStyle(STATUSBAR, BORDER_WIDTH, 0);
    GuiSetStyle(STATUSBAR, TEXT_ALIGNMENT, TEXT_ALIGN_LEFT);
    GuiSetStyle(STATUSBAR, TEXT_PADDING, 10);
    GuiSetStyle(STATUSBAR, BASE_COLOR_NORMAL, ColorToInt(BLANK));

}

/** Draw antialiased text using the shared UI typeface. */
static void drawUiText(const char *text, int x, int y, int size, Color color) {
    const Font font = AppDashboardFont_Regular(&dashboardFonts, size);
    DrawTextEx(font, text, (Vector2){(float)x, (float)y},
               (float)AppDashboardFont_RenderSize(size), 0.0f, color);
}

/** Draw title and metric text using the technical display face. */
static void drawDisplayText(const char *text, int x, int y, int size, Color color) {
    const Font font = AppDashboardFont_Bold(&dashboardFonts, size);
    DrawTextEx(font, text, (Vector2){(float)x, (float)y},
               (float)AppDashboardFont_RenderSize(size), 0.5f, color);
}

/** Draw one rounded dashboard panel with an optional section title. */
static void drawPanel(Rectangle bounds, const char *title) {
    DrawRectangleRounded(bounds, 0.08f, 10, COLOR_PANEL);
    DrawRectangleRoundedLinesEx(bounds, 0.08f, 10, 1.0f, COLOR_BORDER);
    if (title != NULL) {
        drawDisplayText(title, (int)bounds.x + 20, (int)bounds.y + 15,
                        14, COLOR_MUTED);
    }
}

/** Draw the shared slightly-rounded field surface. */
static void drawFieldSurface(Rectangle bounds, bool focused) {
    DrawRectangleRounded(bounds, 16.0f / bounds.height, 12, COLOR_PANEL_RAISED);
    DrawRectangleRoundedLinesEx(bounds, 16.0f / bounds.height, 12, 1.0f,
                                focused ? COLOR_CYAN : COLOR_BORDER);
}

/** Draw one left-aligned, non-editable field. */
static void drawStatusField(Rectangle bounds, const char *text) {
    drawFieldSurface(bounds, false);
    GuiStatusBar((Rectangle){bounds.x + 1.0f, bounds.y + 1.0f,
                             bounds.width - 2.0f, bounds.height - 2.0f}, text);
}

/** Draw the shared rounded receive-mode toggle and report a state change. */
static bool toggleControl(Rectangle bounds, const char *label,
                          bool *value, bool enabled) {
    const bool hovered = enabled && CheckCollisionPointRec(GetMousePosition(), bounds);
    const Rectangle track = {bounds.x, bounds.y + 2.0f, 38.0f, 20.0f};
    const Color trackColor = *value && enabled ? COLOR_CYAN
        : hovered ? (Color){50, 72, 111, 255} : (Color){37, 52, 81, 255};
    DrawRectangleRounded(track, 1.0f, 12, trackColor);
    DrawCircle((int)(track.x + (*value ? 29.0f : 9.0f)),
               (int)(track.y + 10.0f), 8.0f,
               *value && enabled ? (Color){7, 22, 28, 255} : COLOR_MUTED);
    drawUiText(label, (int)bounds.x + 46, (int)bounds.y + 2, 15,
               enabled ? COLOR_TEXT : (Color){94, 107, 127, 255});
    if (hovered && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
        *value = !*value;
        return true;
    }
    return false;
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

/** One runtime receive-buffer choice. */
typedef struct {
    /** Human-readable size. */
    const char *label;
    /** Capacity in bytes. */
    uint32_t size;
} BufferSizePreset_t;

static const Preset_t presets[] = {
    {"HCI Reset",           0x0C03U, {0U},             0U},
    {"Read Local Version",  0x1001U, {0U},             0U},
    {"Read BD_ADDR",        0x1009U, {0U},             0U},
    {"LE Read Buffer Size", 0x2002U, {0U},             0U},
    {"LE Enable Scan",      0x200CU, {0x01U, 0x00U},  2U},
    {"LE Disable Scan",     0x200CU, {0x00U, 0x00U},  2U},
};
static const char *const presetOptions[] = {
    "HCI Reset", "Read Local Version", "Read BD_ADDR",
    "LE Read Buffer Size", "LE Enable Scan", "LE Disable Scan",
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

static const char *const bulkSizeOptions[] = {
    "16 KiB", "64 KiB", "128 KiB", "512 KiB", "1 MiB",
    "2 MiB", "4 MiB", "8 MiB", "16 MiB",
};
static const BufferSizePreset_t ringBufferPresets[] = {
    {"256 B", 256U}, {"1 KiB", 1024U}, {"4 KiB", 4096U},
    {"16 KiB", 16U * 1024U}, {"64 KiB", 64U * 1024U},
};
static const char *const ringBufferOptions[] = {
    "256 B", "1 KiB", "4 KiB", "16 KiB", "64 KiB",
};
static const BufferSizePreset_t dmaBufferPresets[] = {
    {"32 B", 32U}, {"64 B", 64U}, {"128 B", 128U}, {"256 B", 256U},
    {"512 B", 512U}, {"1 KiB", 1024U}, {"4 KiB", 4096U},
};
static const char *const dmaBufferOptions[] = {
    "32 B", "64 B", "128 B", "256 B", "512 B", "1 KiB", "4 KiB",
};

/** Retained scrollable server log. */
typedef AppDashboardLog_t ServerLog_t;

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
    /** RX packet total hidden by the most recent dashboard reset. */
    unsigned int receivedCommandsBaseline;
    /** TX activity total hidden by the most recent dashboard reset. */
    unsigned int transmittedEventsBaseline;
    /** RX interrupt total hidden by the most recent dashboard reset. */
    uint32_t rxInterruptBaseline;
    /** Logical bytes sent by a dashboard-originated test transfer. */
    uint32_t testSent;
    /** Requested dashboard-originated test-transfer size. */
    uint32_t testTotal;
    /** Whether the server is currently generating test chunks. */
    bool testActive;
    /** Active circular ring capacity. */
    uint32_t ringBufferSize;
    /** Active simulated RX DMA transaction size. */
    uint32_t dmaBufferSize;
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
    drawFieldSurface(bounds, *editing);
    const Rectangle content = {bounds.x + 1.0f, bounds.y + 1.0f,
                               bounds.width - 2.0f, bounds.height - 2.0f};
    if (GuiTextBox(content, text, (int)capacity, shortcut ? false : *editing)) {
        *editing = !*editing;
    }
}

/** Append one server log line. */
static void appendLog(ServerLog_t *log, const char *line) {
    AppDashboardLog_Append(log, "%s", line);
}

/** Select the established traffic color for one retained server log line. */
static Color serverLogColor(const char *line) {
    return strncmp(line, "ERROR", 5U) == 0 ? (Color){255, 120, 120, 255}
        : strncmp(line, "RX DMA", 6U) == 0 ? (Color){100, 190, 255, 255}
        : strncmp(line, "TX DMA", 6U) == 0 ? (Color){105, 225, 150, 255}
        : (Color){190, 198, 210, 255};
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
    } else if (sscanf(line, "RX BUFFERS -> ring=%u dma=%u",
                      &parsed, &total) == 2) {
        state->ringBufferSize = parsed;
        state->dmaBufferSize = total;
        return;
    } else if (sscanf(line,
                      "RX IRQ PROGRESS -> absolute=%u transmission=%u",
                      &parsed, &transmissionIrqs) == 2) {
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
    state->receivedCommandsBaseline = 0U;
    state->transmittedEventsBaseline = 0U;
    state->rxInterruptBaseline = 0U;
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

/** Render one dashboard metric card and report a completed click. */
static bool drawMetricCard(Rectangle bounds, const char *label,
                           const char *value, Color accent) {
    const bool hovered = CheckCollisionPointRec(GetMousePosition(), bounds);
    DrawRectangleRounded(bounds, 0.30f, 12, COLOR_PANEL_RAISED);
    if (hovered) {
        DrawRectangleRoundedLinesEx(bounds, 0.30f, 12, 1.0f, accent);
    }
    DrawRectangleRounded((Rectangle){bounds.x + 18.0f, bounds.y + 14.0f,
                                     4.0f, 55.0f},
                         1.0f, 8, accent);
    drawDisplayText(label, (int)bounds.x + 34, (int)bounds.y + 14, 12, COLOR_MUTED);
    drawDisplayText(value, (int)bounds.x + 34, (int)bounds.y + 42, 22, COLOR_TEXT);
    return hovered && IsMouseButtonReleased(MOUSE_BUTTON_LEFT);
}

/** Draw a rounded high-contrast action button and report a completed click. */
static bool brightButton(Rectangle bounds, const char *label, bool enabled) {
    const bool hovered = enabled && CheckCollisionPointRec(GetMousePosition(), bounds);
    const bool pressed = hovered && IsMouseButtonDown(MOUSE_BUTTON_LEFT);
    const Color fill = !enabled ? (Color){39, 49, 67, 255}
        : pressed ? (Color){34, 197, 181, 255}
        : hovered ? (Color){103, 232, 249, 255}
        : (Color){45, 212, 191, 255};
    DrawRectangleRounded(bounds, 16.0f / bounds.height, 12, fill);
    const int fontSize = AppDashboardFont_RenderSize(16);
    const Font font = AppDashboardFont_Bold(&dashboardFonts, 16);
    AppDashboardFont_DrawCentered(
        font, label, bounds, (float)fontSize,
        enabled ? (Color){7, 22, 28, 255} : (Color){94, 107, 127, 255});
    return enabled && hovered && IsMouseButtonReleased(MOUSE_BUTTON_LEFT);
}

/** Run the graphical server dashboard and child-process supervisor. */
int main(void) {
    char windowTitle[96];
    (void)snprintf(windowTitle, sizeof(windowTitle), "BLE UART Server v%s",
                   UART_DMA_APP_VERSION);
    SetConfigFlags(FLAG_WINDOW_HIGHDPI | FLAG_MSAA_4X_HINT);
    InitWindow(1120, 960, windowTitle);
    SetWindowMinSize(980, 820);
    SetTargetFPS(60);
    loadUiFont();

    ServerProcess_t process = {0, -1, -1, false, false, {0}, 0U};
    ServerState_t state = {0};
    ServerLog_t log;
    AppDashboardLog_Init(&log);
    char opcodeText[16] = "0C03";
    char parametersText[768] = "";
    char rawText[1024] = "04 0E 04 01 03 0C 00";
    bool opcodeEdit = false;
    bool parametersEdit = false;
    bool rawEdit = false;
    bool presetDropdown = false;
    bool bulkSizeDropdown = false;
    bool ringSizeDropdown = false;
    bool dmaSizeDropdown = false;
    int presetIndex = 0;
    int previousPreset = 0;
    int bulkSizeIndex = 5;
    int ringSizeIndex = 1;
    int dmaSizeIndex = 3;
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
        ClearBackground(COLOR_CANVAS);
        char heading[96];
        (void)snprintf(heading, sizeof(heading), "BLE UART DMA Server  v%s",
                       UART_DMA_APP_VERSION);
        drawDisplayText(heading, 28, 22, 28, COLOR_TEXT);
        drawUiText("FREERTOS ENGINE  /  CIRCULAR RX DMA  /  LIVE HCI TELEMETRY",
                   30, 62, 12, COLOR_MUTED);

        const Color serverColor = process.running
            ? (process.stopping ? (Color){240, 170, 60, 255} : (Color){45, 185, 105, 255})
            : (Color){145, 152, 166, 255};
        char rxValue[32];
        char txValue[32];
        char irqValue[32];
        const unsigned int visibleRx = state.receivedCommands >= state.receivedCommandsBaseline
            ? state.receivedCommands - state.receivedCommandsBaseline : state.receivedCommands;
        const unsigned int visibleTx = state.transmittedEvents >= state.transmittedEventsBaseline
            ? state.transmittedEvents - state.transmittedEventsBaseline : state.transmittedEvents;
        const uint32_t visibleIrq = state.rxInterruptCount >= state.rxInterruptBaseline
            ? state.rxInterruptCount - state.rxInterruptBaseline : state.rxInterruptCount;
        (void)snprintf(rxValue, sizeof(rxValue), "%u", visibleRx);
        (void)snprintf(txValue, sizeof(txValue), "%u", visibleTx);
        (void)snprintf(irqValue, sizeof(irqValue), "%u",
                       (unsigned int)visibleIrq);
        if (drawMetricCard((Rectangle){24, 100, 259, 84}, "RX PACKETS",
                           rxValue, COLOR_BLUE)) {
            state.receivedCommandsBaseline = state.receivedCommands;
        }
        if (drawMetricCard((Rectangle){295, 100, 259, 84}, "TX ACTIVITY",
                           txValue, COLOR_CYAN)) {
            state.transmittedEventsBaseline = state.transmittedEvents;
        }
        if (drawMetricCard((Rectangle){566, 100, 259, 84}, "RX IRQ CALLS",
                           irqValue, COLOR_PURPLE)) {
            state.rxInterruptBaseline = state.rxInterruptCount;
        }
        (void)drawMetricCard((Rectangle){837, 100, 259, 84}, "TRANSPORT",
                             process.running
                                 ? (state.byteIrqMode ? "BYTE IRQ" : "DMA + IDLE")
                                 : "OFFLINE",
                             serverColor);

        drawPanel((Rectangle){24, 204, 1072, 136}, "SERVER CONTROL");
        DrawCircle(50, 255, 7.0f, serverColor);
        drawUiText(process.running ? (process.stopping ? "Stopping engine" : "Server running")
                                   : "Server stopped",
                   68, 244, 18, COLOR_TEXT);
        drawUiText("Ring", 430, 247, 12, COLOR_MUTED);
        drawUiText("DMA", 620, 247, 12, COLOR_MUTED);
        if (brightButton((Rectangle){876, 232, 200, 40},
                         process.running ? "Stop server" : "Start server", true)) {
            if (process.running) {
                requestServerStop(&process, &log);
            } else {
                if (startServer(&process, &log, &state)) {
                    char command[48];
                    (void)snprintf(command, sizeof(command), "ring %u\n",
                                   (unsigned int)ringBufferPresets[ringSizeIndex].size);
                    (void)sendServerCommand(&process, command);
                    (void)snprintf(command, sizeof(command), "dma-buffer %u\n",
                                   (unsigned int)dmaBufferPresets[dmaSizeIndex].size);
                    (void)sendServerCommand(&process, command);
                }
            }
        }
        GuiLabel((Rectangle){44, 289, 122, 38}, "Pseudo-UART");
        drawStatusField((Rectangle){170, 289, 570, 38},
                        state.port[0] != '\0'
                            ? state.port : "Start the server to create a port");
        if (brightButton((Rectangle){758, 289, 152, 38}, "Copy port",
                         state.port[0] != '\0')) {
            if (state.port[0] != '\0') {
                SetClipboardText(state.port);
                appendLog(&log, "Copied pseudo-UART port to clipboard");
            }
        }
        const bool previousMode = state.byteIrqMode;
        const bool modeChanged = toggleControl(
            (Rectangle){942, 296, 134, 24}, "Per-byte IRQ",
            &state.byteIrqMode, process.running);
        if (modeChanged && !sendServerMode(&process, state.byteIrqMode)) {
            state.byteIrqMode = previousMode;
            appendLog(&log, "ERROR  Unable to send receive-mode request");
        }

        drawPanel((Rectangle){24, 360, 1072, 216}, "HCI COMMAND BUILDER");
        GuiLabel((Rectangle){44, 397, 64, 38}, "Preset");
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
        if (!process.running) {
            GuiDisable();
        }
        const bool sendCommandPressed = brightButton(
            (Rectangle){850, 452, 226, 38}, "Send command", process.running);
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
        GuiLabel((Rectangle){44, 512, 132, 38}, "Raw H4 bytes");
        drawClipboardTextBox((Rectangle){180, 512, 646, 38}, rawText,
                             sizeof(rawText), &rawEdit);
        if (!process.running) {
            GuiDisable();
        }
        const bool sendRawPressed = brightButton(
            (Rectangle){850, 512, 226, 38}, "Append CRC + send", process.running);
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

        drawPanel((Rectangle){24, 596, 1072, 124}, "TEST TRANSMISSIONS");
        if (!process.running || state.testActive) {
            GuiDisable();
        }
        const bool sendSmallTest = brightButton(
            (Rectangle){44, 638, 220, 42}, "Send 128-byte packet",
            process.running && !state.testActive);
        GuiLabel((Rectangle){292, 640, 92, 38}, "Block size");
        const bool sendLargeTest = brightButton(
            (Rectangle){548, 638, 218, 42}, "Send selected block",
            process.running && !state.testActive);
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
        DrawRectangleRounded((Rectangle){792, 638, 284, 16}, 1.0f, 12,
                             (Color){35, 45, 65, 255});
        DrawRectangleRounded((Rectangle){792, 638, 284.0f * progress, 16},
                             1.0f, 12, COLOR_BLUE);
        char progressText[96];
        (void)snprintf(progressText, sizeof(progressText), "%u / %u bytes",
                       (unsigned int)state.testSent,
                       (unsigned int)state.testTotal);
        drawUiText(progressText, 792, 665, 12, COLOR_MUTED);

        drawPanel((Rectangle){24, 740, 1072, 196}, "LIVE DMA / HCI TRAFFIC");
        DrawRectangleRounded((Rectangle){44, 777, 1032, 142}, 16.0f / 142.0f, 12,
                             (Color){7, 11, 20, 255});
        AppDashboardLog_Draw(&log, (Rectangle){58, 786, 1004, 135},
                             AppDashboardFont_Regular(&dashboardFonts, 12),
                             (float)AppDashboardFont_RenderSize(12),
                             15.0f, serverLogColor);

        /* Draw dropdowns last so their expanded lists overlay lower panels. */
        (void)AppDashboard_UpwardDropdown(
            (Rectangle){112, 397, 360, 38}, uiFont, presetOptions,
            sizeof(presetOptions) / sizeof(presetOptions[0]),
            &presetIndex, &presetDropdown);
        (void)AppDashboard_UpwardDropdown(
            (Rectangle){392, 638, 132, 42}, uiFont, bulkSizeOptions,
            sizeof(bulkSizeOptions) / sizeof(bulkSizeOptions[0]),
            &bulkSizeIndex, &bulkSizeDropdown);
        const bool ringSizeChanged = AppDashboard_UpwardDropdown(
            (Rectangle){470, 238, 120, 34}, uiFont, ringBufferOptions,
            sizeof(ringBufferOptions) / sizeof(ringBufferOptions[0]),
            &ringSizeIndex, &ringSizeDropdown);
        const bool dmaSizeChanged = AppDashboard_UpwardDropdown(
            (Rectangle){660, 238, 120, 34}, uiFont, dmaBufferOptions,
            sizeof(dmaBufferOptions) / sizeof(dmaBufferOptions[0]),
            &dmaSizeIndex, &dmaSizeDropdown);
        if (process.running && ringSizeChanged) {
            char command[48];
            (void)snprintf(command, sizeof(command), "ring %u\n",
                           (unsigned int)ringBufferPresets[ringSizeIndex].size);
            (void)sendServerCommand(&process, command);
        }
        if (process.running && dmaSizeChanged) {
            char command[48];
            (void)snprintf(command, sizeof(command), "dma-buffer %u\n",
                           (unsigned int)dmaBufferPresets[dmaSizeIndex].size);
            (void)sendServerCommand(&process, command);
        }
        EndDrawing();
    }

    if (process.running) {
        requestServerStop(&process, &log);
        (void)waitpid(process.processId, NULL, 0);
        close(process.outputDescriptor);
        close(process.inputDescriptor);
    }
    AppDashboardFont_Unload(&dashboardFonts);
    CloseWindow();
    return 0;
}
