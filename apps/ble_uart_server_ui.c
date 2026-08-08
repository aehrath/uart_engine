/** @file
 * @brief Portable graphical supervisor for the continuous BLE UART server. */
#include "raylib.h"

#define RAYGUI_IMPLEMENTATION
#include "raygui.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_LOG_LINES       13U
#define MAX_LOG_LENGTH      220U
#define PROCESS_BUFFER_SIZE 2048U

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
} ServerState_t;

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
    if (strncmp(line, "RX DMA -> HCI command", 21U) == 0 ||
        strstr(line, "TEST message start") != NULL) {
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
    int outputPipe[2];
    if (pipe(outputPipe) != 0) {
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
        appendLog(log, "ERROR  Unable to start BLE server process");
        return false;
    }
    if (child == 0) {
        close(outputPipe[0]);
        (void)dup2(outputPipe[1], STDOUT_FILENO);
        (void)dup2(outputPipe[1], STDERR_FILENO);
        close(outputPipe[1]);
        execl(helperPath, helperPath, (char *)NULL);
        perror("Unable to launch bundled BLE server engine");
        _exit(127);
    }

    close(outputPipe[1]);
    (void)fcntl(outputPipe[0], F_SETFL, fcntl(outputPipe[0], F_GETFL) | O_NONBLOCK);
    process->processId = child;
    process->outputDescriptor = outputPipe[0];
    process->running = true;
    process->stopping = false;
    process->partialLength = 0U;
    state->port[0] = '\0';
    state->receivedCommands = 0U;
    state->transmittedEvents = 0U;
    state->exitStatus = 0;
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
        process->running = false;
        process->stopping = false;
        state->exitStatus = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        appendLog(log, state->exitStatus == 0
            ? "BLE server stopped cleanly"
            : "ERROR  BLE server engine exited unexpectedly");
    }
}

/** Render one dashboard metric card. */
static void drawMetricCard(Rectangle bounds, const char *label,
                           const char *value, Color accent) {
    DrawRectangleRounded(bounds, 0.10f, 8, WHITE);
    DrawRectangleRounded((Rectangle){bounds.x, bounds.y, 5.0f, bounds.height},
                         0.5f, 4, accent);
    DrawText(label, (int)bounds.x + 18, (int)bounds.y + 14, 14,
             (Color){100, 110, 128, 255});
    DrawText(value, (int)bounds.x + 18, (int)bounds.y + 40, 24,
             (Color){25, 35, 55, 255});
}

/** Run the graphical server dashboard and child-process supervisor. */
int main(void) {
    InitWindow(960, 700, "BLE UART Server");
    SetWindowMinSize(760, 600);
    SetTargetFPS(60);

    ServerProcess_t process = {0, -1, false, false, {0}, 0U};
    ServerState_t state = {{0}, 0U, 0U, 0};
    ServerLog_t log = {{{0}}, 0U};
    appendLog(&log, "Ready. Start the server to create a local pseudo-UART.");

    while (!WindowShouldClose()) {
        pollServer(&process, &log, &state);

        BeginDrawing();
        ClearBackground((Color){245, 247, 250, 255});
        DrawText("BLE UART DMA Server", 24, 20, 26, (Color){25, 35, 55, 255});
        DrawText("FreeRTOS • circular RX DMA • HCI command responder",
                 24, 52, 16, (Color){90, 100, 120, 255});

        const Color serverColor = process.running
            ? (process.stopping ? (Color){240, 170, 60, 255} : (Color){45, 185, 105, 255})
            : (Color){145, 152, 166, 255};
        DrawCircle(35, 110, 7.0f, serverColor);
        DrawText(process.running ? (process.stopping ? "Stopping" : "Server running")
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
        (void)snprintf(rxValue, sizeof(rxValue), "%u", state.receivedCommands);
        (void)snprintf(txValue, sizeof(txValue), "%u", state.transmittedEvents);
        drawMetricCard((Rectangle){24, 151, 284, 82}, "RX messages", rxValue,
                       (Color){70, 145, 235, 255});
        drawMetricCard((Rectangle){327, 151, 284, 82}, "TX Command Complete events", txValue,
                       (Color){45, 185, 105, 255});
        drawMetricCard((Rectangle){630, 151, 306, 82}, "Transport",
                       process.running ? "DMA active" : "Idle",
                       serverColor);

        GuiGroupBox((Rectangle){20, 253, 920, 103}, "Client connection");
        GuiLabel((Rectangle){36, 275, 125, 24}, "Pseudo-UART port");
        GuiStatusBar((Rectangle){163, 272, 568, 30},
                     state.port[0] != '\0' ? state.port : "Start the server to create a port");
        if (GuiButton((Rectangle){748, 272, 168, 30}, "Copy port")) {
            if (state.port[0] != '\0') {
                SetClipboardText(state.port);
                appendLog(&log, "Copied pseudo-UART port to clipboard");
            }
        }
        GuiLabel((Rectangle){36, 315, 880, 24},
                 "Paste this port into the Python app, C terminal client, or portable C client UI.");

        GuiGroupBox((Rectangle){20, 374, 920, 306}, "Live DMA / HCI traffic");
        DrawRectangle(34, 398, 892, 266, (Color){20, 25, 35, 255});
        for (size_t index = 0U; index < log.count; index++) {
            const Color color = strncmp(log.lines[index], "ERROR", 5U) == 0
                ? (Color){255, 120, 120, 255}
                : strncmp(log.lines[index], "RX DMA", 6U) == 0
                    ? (Color){100, 190, 255, 255}
                    : strncmp(log.lines[index], "TX DMA", 6U) == 0
                        ? (Color){105, 225, 150, 255}
                        : (Color){190, 198, 210, 255};
            DrawText(log.lines[index], 46, 410 + (int)index * 19, 14, color);
        }
        EndDrawing();
    }

    if (process.running) {
        requestServerStop(&process, &log);
        (void)waitpid(process.processId, NULL, 0);
        close(process.outputDescriptor);
    }
    CloseWindow();
    return 0;
}
