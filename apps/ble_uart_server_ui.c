/** @file
 * @brief Portable graphical supervisor for the continuous BLE UART server. */
#include "raylib.h"
#include "dashboard_config.h"
#include "dashboard_dropdown.h"
#include "dashboard_log.h"
#include "dashboard_ui.h"
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
    /** Configured KW47 LPUART source clock. */
    uint32_t systemClockHz;
    /** Requested UART line rate. */
    uint32_t requestedBaud;
    /** Selected effective oversampling ratio. */
    uint32_t osr;
    /** Selected baud-rate modulo divisor. */
    uint32_t sbr;
    /** Rounded baud produced by the selected divisors. */
    uint32_t actualBaud;
} ServerState_t;

/** Append one server log line. */
static void appendLog(ServerLog_t *log, const char *line) {
    AppDashboardLog_Append(log, "%s", line);
}

/** Select the established traffic color for one retained server log line. */
static Color serverLogColor(const char *line) {
    return strncmp(line, "ERROR", 5U) == 0 ? (Color){255, 120, 120, 255}
        : strncmp(line, "WARNING", 7U) == 0 ? (Color){255, 196, 92, 255}
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
                      "UART TIMING -> clock=%u requested=%u osr=%u sbr=%u actual=%u",
                      &state->systemClockHz, &state->requestedBaud, &state->osr,
                      &state->sbr, &state->actualBaud) == 5) {
        appendLog(log, line);
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

/** Run the graphical server dashboard and child-process supervisor. */
int main(void) {
    char windowTitle[96];
    (void)snprintf(windowTitle, sizeof(windowTitle), "BLE UART Server v%s",
                   UART_DMA_APP_VERSION);
    SetConfigFlags(FLAG_WINDOW_HIGHDPI | FLAG_MSAA_4X_HINT);
    InitWindow(1120, 960, windowTitle);
    SetWindowMinSize(980, 820);
    SetTargetFPS(60);
    AppDashboardUi_Init(UART_DMA_UI_FONT_FILE,
                        UART_DMA_UI_DISPLAY_FONT_FILE);

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
    bool clockDropdown = false;
    bool baudDropdown = false;
    int presetIndex = 0;
    int previousPreset = 0;
    int bulkSizeIndex = 5;
    int ringSizeIndex = 1;
    int dmaSizeIndex = 3;
    int clockIndex = 4;
    int baudIndex = 4;
    unsigned int statsFrame = 0U;
    AppDashboardConfig_PresetToFields(0, opcodeText, sizeof(opcodeText),
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
        AppDashboardUi_DrawDisplayText(heading, 28, 22, 28, COLOR_TEXT);
        AppDashboardUi_DrawText("FREERTOS ENGINE  /  CIRCULAR RX DMA  /  LIVE HCI TELEMETRY",
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
        if (AppDashboardUi_MetricCard((Rectangle){24, 100, 259, 84}, "RX PACKETS",
                           rxValue, COLOR_BLUE)) {
            state.receivedCommandsBaseline = state.receivedCommands;
        }
        if (AppDashboardUi_MetricCard((Rectangle){295, 100, 259, 84}, "TX ACTIVITY",
                           txValue, COLOR_CYAN)) {
            state.transmittedEventsBaseline = state.transmittedEvents;
        }
        if (AppDashboardUi_MetricCard((Rectangle){566, 100, 259, 84}, "RX IRQ CALLS",
                           irqValue, COLOR_PURPLE)) {
            state.rxInterruptBaseline = state.rxInterruptCount;
        }
        (void)AppDashboardUi_MetricCard((Rectangle){837, 100, 259, 84}, "TRANSPORT",
                             process.running
                                 ? (state.byteIrqMode ? "BYTE IRQ" : "DMA + IDLE")
                                 : "OFFLINE",
                             serverColor);

        AppDashboardUi_DrawPanel((Rectangle){24, 204, 1072, 136}, "SERVER CONTROL");
        DrawCircle(50, 255, 7.0f, serverColor);
        AppDashboardUi_DrawText(process.running ? (process.stopping ? "Stopping engine" : "Server running")
                                   : "Server stopped",
                   68, 244, 18, COLOR_TEXT);
        AppDashboardUi_DrawText("Clock", 230, 247, 12, COLOR_MUTED);
        AppDashboardUi_DrawText("Baud", 390, 247, 12, COLOR_MUTED);
        AppDashboardUi_DrawText("Ring", 548, 247, 12, COLOR_MUTED);
        AppDashboardUi_DrawText("DMA", 706, 247, 12, COLOR_MUTED);
        if (AppDashboardUi_Button((Rectangle){900, 232, 176, 40},
                         process.running ? "Stop server" : "Start server", true)) {
            if (process.running) {
                requestServerStop(&process, &log);
            } else {
                if (startServer(&process, &log, &state)) {
                    char command[48];
                    (void)snprintf(command, sizeof(command), "ring %u\n",
                                   (unsigned int)APP_DASHBOARD_RING_SIZES[ringSizeIndex].size);
                    (void)sendServerCommand(&process, command);
                    (void)snprintf(command, sizeof(command), "dma-buffer %u\n",
                                   (unsigned int)APP_DASHBOARD_DMA_SIZES[dmaSizeIndex].size);
                    (void)sendServerCommand(&process, command);
                    (void)snprintf(command, sizeof(command), "clock %u\n",
                                   (unsigned int)APP_DASHBOARD_SYSTEM_CLOCKS[clockIndex].size);
                    (void)sendServerCommand(&process, command);
                    (void)snprintf(command, sizeof(command), "baud %u\n",
                                   (unsigned int)APP_DASHBOARD_BAUD_RATES[baudIndex].size);
                    (void)sendServerCommand(&process, command);
                }
            }
        }
        GuiLabel((Rectangle){44, 289, 122, 38}, "Pseudo-UART");
        AppDashboardUi_StatusField((Rectangle){170, 289, 570, 38},
                        state.port[0] != '\0'
                            ? state.port : "Start the server to create a port");
        if (AppDashboardUi_Button((Rectangle){758, 289, 152, 38}, "Copy port",
                         state.port[0] != '\0')) {
            if (state.port[0] != '\0') {
                SetClipboardText(state.port);
                appendLog(&log, "Copied pseudo-UART port to clipboard");
            }
        }
        const bool previousMode = state.byteIrqMode;
        const bool modeChanged = AppDashboardUi_Toggle(
            (Rectangle){942, 296, 134, 24}, "Per-byte IRQ",
            &state.byteIrqMode, process.running);
        if (modeChanged && !sendServerMode(&process, state.byteIrqMode)) {
            state.byteIrqMode = previousMode;
            appendLog(&log, "ERROR  Unable to send receive-mode request");
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
        if (!process.running) {
            GuiDisable();
        }
        const bool sendCommandPressed = AppDashboardUi_Button(
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
        AppDashboardUi_TextBox((Rectangle){180, 512, 646, 38}, rawText,
                             sizeof(rawText), &rawEdit);
        if (!process.running) {
            GuiDisable();
        }
        const bool sendRawPressed = AppDashboardUi_Button(
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

        AppDashboardUi_DrawPanel((Rectangle){24, 596, 1072, 124}, "TEST TRANSMISSIONS");
        if (!process.running || state.testActive) {
            GuiDisable();
        }
        const bool sendSmallTest = AppDashboardUi_Button(
            (Rectangle){44, 638, 220, 42}, "Send 128-byte packet",
            process.running && !state.testActive);
        GuiLabel((Rectangle){292, 640, 92, 38}, "Block size");
        const bool sendLargeTest = AppDashboardUi_Button(
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
                                  APP_DASHBOARD_BULK_SIZES[bulkSizeIndex].size);
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
        AppDashboardUi_DrawText(progressText, 792, 665, 12, COLOR_MUTED);

        AppDashboardUi_DrawPanel((Rectangle){24, 740, 1072, 196}, "LIVE DMA / HCI TRAFFIC");
        DrawRectangleRounded((Rectangle){44, 777, 1032, 142}, 16.0f / 142.0f, 12,
                             (Color){7, 11, 20, 255});
        AppDashboardLog_Draw(&log, (Rectangle){58, 786, 1004, 135},
                             AppDashboardUi_RegularFont(12),
                             (float)AppDashboardUi_RenderSize(12),
                             15.0f, serverLogColor);

        /* Draw dropdowns last so their expanded lists overlay lower panels. */
        (void)AppDashboard_UpwardDropdown(
            (Rectangle){112, 397, 360, 38}, AppDashboardUi_Font(), APP_DASHBOARD_PRESET_OPTIONS,
            APP_DASHBOARD_PRESET_COUNT,
            &presetIndex, &presetDropdown);
        (void)AppDashboard_UpwardDropdown(
            (Rectangle){392, 638, 132, 42}, AppDashboardUi_Font(), APP_DASHBOARD_BULK_SIZE_OPTIONS,
            APP_DASHBOARD_BULK_SIZE_COUNT,
            &bulkSizeIndex, &bulkSizeDropdown);
        const bool ringSizeChanged = AppDashboard_UpwardDropdown(
            (Rectangle){582, 238, 104, 34}, AppDashboardUi_Font(), APP_DASHBOARD_RING_SIZE_OPTIONS,
            APP_DASHBOARD_RING_SIZE_COUNT,
            &ringSizeIndex, &ringSizeDropdown);
        const bool dmaSizeChanged = AppDashboard_UpwardDropdown(
            (Rectangle){742, 238, 104, 34}, AppDashboardUi_Font(), APP_DASHBOARD_DMA_SIZE_OPTIONS,
            APP_DASHBOARD_DMA_SIZE_COUNT,
            &dmaSizeIndex, &dmaSizeDropdown);
        const bool clockChanged = AppDashboard_UpwardDropdown(
            (Rectangle){276, 238, 104, 34}, AppDashboardUi_Font(),
            APP_DASHBOARD_SYSTEM_CLOCK_OPTIONS, APP_DASHBOARD_SYSTEM_CLOCK_COUNT,
            &clockIndex, &clockDropdown);
        const bool baudChanged = AppDashboard_UpwardDropdown(
            (Rectangle){430, 238, 104, 34}, AppDashboardUi_Font(),
            APP_DASHBOARD_BAUD_RATE_OPTIONS, APP_DASHBOARD_BAUD_RATE_COUNT,
            &baudIndex, &baudDropdown);
        if (process.running && ringSizeChanged) {
            char command[48];
            (void)snprintf(command, sizeof(command), "ring %u\n",
                           (unsigned int)APP_DASHBOARD_RING_SIZES[ringSizeIndex].size);
            (void)sendServerCommand(&process, command);
        }
        if (process.running && dmaSizeChanged) {
            char command[48];
            (void)snprintf(command, sizeof(command), "dma-buffer %u\n",
                           (unsigned int)APP_DASHBOARD_DMA_SIZES[dmaSizeIndex].size);
            (void)sendServerCommand(&process, command);
        }
        if (process.running && clockChanged) {
            char command[48];
            (void)snprintf(command, sizeof(command), "clock %u\n",
                           (unsigned int)APP_DASHBOARD_SYSTEM_CLOCKS[clockIndex].size);
            (void)sendServerCommand(&process, command);
        }
        if (process.running && baudChanged) {
            char command[48];
            (void)snprintf(command, sizeof(command), "baud %u\n",
                           (unsigned int)APP_DASHBOARD_BAUD_RATES[baudIndex].size);
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
    AppDashboardUi_Shutdown();
    CloseWindow();
    return 0;
}
