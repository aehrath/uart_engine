/** @file
 * @brief Portable graphical supervisor for the continuous BLE UART server. */
#include "raylib.h"
#include "client_preferences.h"
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
#include <inttypes.h>
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
#define PIPELINE_HISTORY_SIZE 120U


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
    /** Measured producer throughput over the latest sampling interval. */
    uint64_t producerBps;
    /** Measured consumer throughput over the latest sampling interval. */
    uint64_t consumerBps;
    /** Payload bytes currently retained by the packet queue. */
    uint32_t queuedBytes;
    /** Current and maximum packet-queue occupancy. */
    uint32_t queueDepth;
    uint32_t queueCapacity;
    uint32_t queueHighWater;
    /** Latest and maximum observed circular-ring burst. */
    uint32_t ringBurstBytes;
    uint32_t ringHighWaterBytes;
    /** Latest and maximum enqueue-to-consume latency. */
    uint64_t latencyUs;
    uint64_t latencyMaxUs;
    /** Measured time until buffers fill, or UINT64_MAX while stable. */
    uint64_t headroomUs;
    /** Cumulative measured corruption and capacity losses. */
    uint32_t crcDrops;
    uint32_t queueDrops;
    uint32_t ringDrops;
    /** Ten-hertz polls accumulated into each half-second graph sample. */
    uint32_t pipelinePollCount;
    uint64_t producerBpsSum;
    uint64_t consumerBpsSum;
    uint32_t sampleQueueHighWater;
    uint32_t sampleRingHighWaterBytes;
    /** Circular history of timestamped producer/consumer rate samples. */
    float producerHistory[PIPELINE_HISTORY_SIZE];
    float consumerHistory[PIPELINE_HISTORY_SIZE];
    float queueHistory[PIPELINE_HISTORY_SIZE];
    float ringHistory[PIPELINE_HISTORY_SIZE];
    float dmaHistory[PIPELINE_HISTORY_SIZE];
    size_t historyCount;
    size_t historyHead;
} ServerState_t;

/** Convert an interval high-water mark to a chart-safe capacity percentage. */
static float capacityPercent(uint32_t highWater, uint32_t capacity) {
    if (capacity == 0U) return 0.0f;
    const float percentage = 100.0f * (float)highWater / (float)capacity;
    return percentage > 100.0f ? 100.0f : percentage;
}

/** Retain one producer/consumer sample in chronological circular history. */
static void appendPipelineSample(ServerState_t *state) {
    state->producerHistory[state->historyHead] = (float)state->producerBps;
    state->consumerHistory[state->historyHead] = (float)state->consumerBps;
    state->queueHistory[state->historyHead] = capacityPercent(
        state->queueHighWater, state->queueCapacity);
    state->ringHistory[state->historyHead] = capacityPercent(
        state->ringHighWaterBytes, state->ringBufferSize);
    const uint32_t dmaPeak = state->ringHighWaterBytes < state->dmaBufferSize
        ? state->ringHighWaterBytes : state->dmaBufferSize;
    state->dmaHistory[state->historyHead] = capacityPercent(
        dmaPeak, state->dmaBufferSize);
    state->historyHead = (state->historyHead + 1U) % PIPELINE_HISTORY_SIZE;
    if (state->historyCount < PIPELINE_HISTORY_SIZE) {
        state->historyCount++;
    }
}

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
    unsigned int queueDepth = 0U;
    unsigned int queueCapacity = 0U;
    unsigned int queueHighWater = 0U;
    unsigned int ringBurst = 0U;
    unsigned int ringCapacity = 0U;
    unsigned int ringHighWater = 0U;
    unsigned int crcDrops = 0U;
    unsigned int queueDrops = 0U;
    unsigned int ringDrops = 0U;
    uint64_t producerBps = 0U;
    uint64_t consumerBps = 0U;
    uint64_t latencyUs = 0U;
    uint64_t latencyMaxUs = 0U;
    uint64_t headroomUs = UINT64_MAX;
    if (sscanf(line, "RX IRQ COUNT -> %u", &parsed) == 1) {
        state->rxInterruptCount = parsed;
        return;
    } else if (sscanf(line,
                      "RX PIPELINE -> producer_bps=%" SCNu64
                      " consumer_bps=%" SCNu64
                      " queued=%u queue=%u/%u queue_peak=%u"
                      " ring=%u/%u ring_peak=%u latency_us=%" SCNu64
                      " latency_max_us=%" SCNu64 " headroom_us=%" SCNu64
                      " crc_drop=%u queue_drop=%u ring_drop=%u",
                      &producerBps, &consumerBps, &total, &queueDepth,
                      &queueCapacity, &queueHighWater, &ringBurst,
                      &ringCapacity, &ringHighWater, &latencyUs,
                      &latencyMaxUs, &headroomUs, &crcDrops, &queueDrops,
                      &ringDrops) == 15) {
        state->producerBpsSum += producerBps;
        state->consumerBpsSum += consumerBps;
        state->pipelinePollCount++;
        if (queueHighWater > state->sampleQueueHighWater) {
            state->sampleQueueHighWater = queueHighWater;
        }
        if (ringHighWater > state->sampleRingHighWaterBytes) {
            state->sampleRingHighWaterBytes = ringHighWater;
        }
        state->queuedBytes = total;
        state->queueDepth = queueDepth != 0U ? queueDepth : queueHighWater;
        state->queueCapacity = queueCapacity;
        /* The engine consumes each IDLE-delimited burst before this 2 Hz poll.
         * Display its interval peak for one poll so the live gauge is visible. */
        state->ringBurstBytes = ringBurst != 0U ? ringBurst : ringHighWater;
        state->ringBufferSize = ringCapacity;
        state->latencyUs = latencyUs;
        state->latencyMaxUs = latencyMaxUs;
        state->headroomUs = headroomUs;
        state->crcDrops = crcDrops;
        state->queueDrops = queueDrops;
        state->ringDrops = ringDrops;
        if (state->pipelinePollCount >= 5U) {
            state->producerBps = state->producerBpsSum / state->pipelinePollCount;
            state->consumerBps = state->consumerBpsSum / state->pipelinePollCount;
            state->queueHighWater = state->sampleQueueHighWater;
            state->ringHighWaterBytes = state->sampleRingHighWaterBytes;
            appendPipelineSample(state);
            state->pipelinePollCount = 0U;
            state->producerBpsSum = 0U;
            state->consumerBpsSum = 0U;
            state->sampleQueueHighWater = 0U;
            state->sampleRingHighWaterBytes = 0U;
        }
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
    } else if (sscanf(line, "TX DMA <- TEST gauge %u/%u bytes",
                      &parsed, &total) == 2) {
        state->testSent = parsed;
        state->testTotal = total;
        return;
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
    state->producerBps = 0U;
    state->consumerBps = 0U;
    state->queuedBytes = 0U;
    state->queueDepth = 0U;
    state->queueCapacity = 0U;
    state->queueHighWater = 0U;
    state->ringBurstBytes = 0U;
    state->ringHighWaterBytes = 0U;
    state->latencyUs = 0U;
    state->latencyMaxUs = 0U;
    state->headroomUs = UINT64_MAX;
    state->crcDrops = 0U;
    state->queueDrops = 0U;
    state->ringDrops = 0U;
    state->pipelinePollCount = 0U;
    state->producerBpsSum = 0U;
    state->consumerBpsSum = 0U;
    state->sampleQueueHighWater = 0U;
    state->sampleRingHighWaterBytes = 0U;
    memset(state->producerHistory, 0, sizeof(state->producerHistory));
    memset(state->consumerHistory, 0, sizeof(state->consumerHistory));
    memset(state->queueHistory, 0, sizeof(state->queueHistory));
    memset(state->ringHistory, 0, sizeof(state->ringHistory));
    memset(state->dmaHistory, 0, sizeof(state->dmaHistory));
    state->historyCount = 0U;
    state->historyHead = 0U;
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

/** Format a byte rate compactly without hiding the measured value's scale. */
static void formatRate(char *output, size_t capacity, uint64_t bytesPerSecond) {
    if (bytesPerSecond >= 1000000U) {
        (void)snprintf(output, capacity, "%.2f MB/s",
                       (double)bytesPerSecond / 1000000.0);
    } else if (bytesPerSecond >= 1000U) {
        (void)snprintf(output, capacity, "%.1f kB/s",
                       (double)bytesPerSecond / 1000.0);
    } else {
        (void)snprintf(output, capacity, "%" PRIu64 " B/s", bytesPerSecond);
    }
}

/** Format measured latency or remaining time for the compact chart footer. */
static void formatDuration(char *output, size_t capacity, uint64_t microseconds,
                           bool allowStable) {
    if (allowStable && microseconds == UINT64_MAX) {
        (void)snprintf(output, capacity, "stable");
    } else if (microseconds >= 1000000U) {
        (void)snprintf(output, capacity, "%.2f s",
                       (double)microseconds / 1000000.0);
    } else if (microseconds >= 1000U) {
        (void)snprintf(output, capacity, "%.1f ms",
                       (double)microseconds / 1000.0);
    } else {
        (void)snprintf(output, capacity, "%" PRIu64 " us", microseconds);
    }
}

/** Draw one chronological history line using a shared vertical rate scale. */
static void drawHistoryLine(const ServerState_t *state, const float *history,
                            Rectangle bounds, float maximum, float thickness,
                            Color color) {
    if (state->historyCount == 0U || maximum <= 0.0f) {
        return;
    }
    const size_t oldest = (state->historyHead + PIPELINE_HISTORY_SIZE -
                           state->historyCount) % PIPELINE_HISTORY_SIZE;
    for (size_t sample = 1U; sample < state->historyCount; ++sample) {
        const size_t previous = (oldest + sample - 1U) % PIPELINE_HISTORY_SIZE;
        const size_t current = (oldest + sample) % PIPELINE_HISTORY_SIZE;
        const float x0 = bounds.x + bounds.width * (float)(sample - 1U) /
            (float)(PIPELINE_HISTORY_SIZE - 1U);
        const float x1 = bounds.x + bounds.width * (float)sample /
            (float)(PIPELINE_HISTORY_SIZE - 1U);
        const float y0 = bounds.y + bounds.height -
            bounds.height * history[previous] / maximum;
        const float y1 = bounds.y + bounds.height -
            bounds.height * history[current] / maximum;
        DrawLineEx((Vector2){x0, y0}, (Vector2){x1, y1}, thickness, color);
    }
    const size_t newest = (state->historyHead + PIPELINE_HISTORY_SIZE - 1U) %
        PIPELINE_HISTORY_SIZE;
    const float newestX = bounds.x + bounds.width *
        (float)(state->historyCount - 1U) / (float)(PIPELINE_HISTORY_SIZE - 1U);
    const float newestY = bounds.y + bounds.height -
        bounds.height * history[newest] / maximum;
    DrawCircleV((Vector2){newestX, newestY}, 2.5f, color);
}

/** Draw a labeled current-capacity gauge. */
static void drawPipelineGauge(const char *label, uint32_t current,
                              uint32_t capacity, Rectangle bounds,
                              Color color) {
    const float fraction = capacity == 0U ? 0.0f
        : (float)current / (float)capacity;
    char value[64];
    const double percentage = capacity == 0U ? 0.0
        : current >= capacity ? 100.0 : 100.0 * current / capacity;
    (void)snprintf(value, sizeof(value), "%s %.1f%%", label, percentage);
    AppDashboardUi_DrawText(value, (int)bounds.x, (int)bounds.y - 2, 10,
                            COLOR_MUTED);
    const Rectangle track = {bounds.x + 94.0f, bounds.y, bounds.width - 94.0f,
                             bounds.height};
    DrawRectangleRounded(track, 1.0f, 8, (Color){35, 45, 65, 255});
    if (fraction > 0.0f) {
        Rectangle fill = track;
        fill.width *= fraction > 1.0f ? 1.0f : fraction;
        DrawRectangleRounded(fill, 1.0f, 8, color);
    }
}

/** Draw actual producer/consumer rates, pressure, latency, and headroom. */
static void drawPipelineVisualization(const ServerState_t *state,
                                      Rectangle bounds) {
    DrawRectangleRounded(bounds, 16.0f / bounds.height, 12,
                         (Color){7, 11, 20, 255});
    char producer[32];
    char consumer[32];
    formatRate(producer, sizeof(producer), state->producerBps);
    formatRate(consumer, sizeof(consumer), state->consumerBps);
    char legend[96];
    (void)snprintf(legend, sizeof(legend), "PRODUCER  %s     CONSUMER  %s",
                   producer, consumer);
    AppDashboardUi_DrawDisplayText(legend, (int)bounds.x + 12,
                                   (int)bounds.y + 8, 10, COLOR_TEXT);

    const Rectangle graph = {bounds.x + 12.0f, bounds.y + 28.0f,
                             bounds.width - 24.0f, 43.0f};
    DrawRectangleRounded(graph, 0.15f, 8, (Color){12, 19, 33, 255});
    AppDashboardUi_DrawText("Q%", (int)(graph.x + graph.width - 66),
                            (int)graph.y + 2, 9, COLOR_PURPLE);
    AppDashboardUi_DrawText("R%", (int)(graph.x + graph.width - 44),
                            (int)graph.y + 2, 9, (Color){255, 196, 92, 255});
    AppDashboardUi_DrawText("D%", (int)(graph.x + graph.width - 22),
                            (int)graph.y + 2, 9, (Color){145, 152, 166, 255});
    DrawLine((int)graph.x, (int)(graph.y + graph.height * 0.5f),
             (int)(graph.x + graph.width),
             (int)(graph.y + graph.height * 0.5f), COLOR_BORDER);
    float maximum = 1.0f;
    for (size_t sample = 0U; sample < state->historyCount; ++sample) {
        const size_t index = (state->historyHead + PIPELINE_HISTORY_SIZE -
                              state->historyCount + sample) % PIPELINE_HISTORY_SIZE;
        if (state->producerHistory[index] > maximum) maximum = state->producerHistory[index];
        if (state->consumerHistory[index] > maximum) maximum = state->consumerHistory[index];
    }
    drawHistoryLine(state, state->producerHistory, graph, maximum, 2.0f,
                    COLOR_BLUE);
    drawHistoryLine(state, state->consumerHistory, graph, maximum, 2.0f,
                    COLOR_CYAN);
    drawHistoryLine(state, state->dmaHistory, graph, 100.0f, 4.0f,
                    (Color){145, 152, 166, 255});
    drawHistoryLine(state, state->ringHistory, graph, 100.0f, 2.0f,
                    (Color){255, 196, 92, 255});
    drawHistoryLine(state, state->queueHistory, graph, 100.0f, 2.0f,
                    COLOR_PURPLE);

    drawPipelineGauge("Q PEAK", state->queueDepth, state->queueCapacity,
                      (Rectangle){bounds.x + 12.0f, bounds.y + 78.0f,
                                  bounds.width - 24.0f, 8.0f}, COLOR_PURPLE);
    drawPipelineGauge("R PEAK", state->ringBurstBytes, state->ringBufferSize,
                      (Rectangle){bounds.x + 12.0f, bounds.y + 95.0f,
                                  bounds.width - 24.0f, 8.0f}, COLOR_BLUE);

    char latency[24];
    char headroom[24];
    formatDuration(latency, sizeof(latency), state->latencyUs, false);
    formatDuration(headroom, sizeof(headroom), state->headroomUs, true);
    char footer[128];
    (void)snprintf(footer, sizeof(footer),
                   "LATENCY %s   HEADROOM %s   DROPS %u/%u/%u",
                   latency, headroom, (unsigned int)state->crcDrops,
                   (unsigned int)state->queueDrops,
                   (unsigned int)state->ringDrops);
    const Color footerColor = state->headroomUs != UINT64_MAX &&
        state->headroomUs < 2000000U ? (Color){255, 120, 120, 255}
        : state->headroomUs != UINT64_MAX && state->headroomUs < 10000000U
            ? (Color){255, 196, 92, 255} : COLOR_MUTED;
    AppDashboardUi_DrawText(footer, (int)bounds.x + 12,
                            (int)bounds.y + 114, 10, footerColor);
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
    AppDashboardSettings_t savedSettings;
    (void)AppClientPreferences_LoadSettings(
        APP_PREFERENCES_SERVER, &savedSettings);
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
    state.byteIrqMode = savedSettings.byteIrq;
    unsigned int statsFrame = 0U;
    AppDashboardConfig_PresetToFields(0, opcodeText, sizeof(opcodeText),
                   parametersText, sizeof(parametersText));
    appendLog(&log, "Ready. Start the server to create a local pseudo-UART.");

    while (!WindowShouldClose()) {
        pollServer(&process, &log, &state);
        if (process.running && ++statsFrame >= 6U) {
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
                    state.byteIrqMode = savedSettings.byteIrq;
                    (void)sendServerMode(&process, state.byteIrqMode);
                }
            }
        }
        GuiLabel((Rectangle){44, 289, 122, 38}, "Pseudo-UART");
        AppDashboardUi_StatusField((Rectangle){170, 289, 570, 38},
                        state.port[0] != '\0'
                            ? state.port : "Start the server to create a port");
        const bool restoreDefaults = AppDashboardUi_Button(
            (Rectangle){758, 289, 100, 38}, "Defaults", true);
        if (AppDashboardUi_Button((Rectangle){866, 289, 100, 38}, "Copy port",
                         state.port[0] != '\0')) {
            if (state.port[0] != '\0') {
                SetClipboardText(state.port);
                appendLog(&log, "Copied pseudo-UART port to clipboard");
            }
        }
        const bool previousMode = state.byteIrqMode;
        const bool modeChanged = AppDashboardUi_Toggle(
            (Rectangle){974, 296, 102, 24}, "Byte IRQ",
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

        AppDashboardUi_DrawPanel((Rectangle){24, 740, 1072, 196},
                                 "LIVE DMA / HCI TRAFFIC + PIPELINE HEADROOM");
        DrawRectangleRounded((Rectangle){44, 777, 540, 142}, 16.0f / 142.0f, 12,
                             (Color){7, 11, 20, 255});
        AppDashboardLog_Draw(&log, (Rectangle){58, 786, 512, 135},
                             AppDashboardUi_RegularFont(12),
                             (float)AppDashboardUi_RenderSize(12),
                             15.0f, serverLogColor);
        drawPipelineVisualization(&state, (Rectangle){600, 777, 476, 142});

        /* Draw dropdowns last so their expanded lists overlay lower panels. */
        (void)AppDashboard_UpwardDropdown(
            (Rectangle){112, 397, 360, 38}, AppDashboardUi_Font(), APP_DASHBOARD_PRESET_OPTIONS,
            APP_DASHBOARD_PRESET_COUNT,
            &presetIndex, &presetDropdown);
        const bool bulkSizeChanged = AppDashboard_UpwardDropdown(
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
        if (restoreDefaults) {
            AppClientPreferences_DefaultSettings(&savedSettings);
            bulkSizeIndex = 5;
            ringSizeIndex = 6;
            dmaSizeIndex = 3;
            clockIndex = 4;
            baudIndex = 4;
            state.byteIrqMode = false;
            if (process.running) {
                (void)sendServerCommand(&process, "ring 16384\n");
                (void)sendServerCommand(&process, "dma-buffer 256\n");
                (void)sendServerCommand(&process, "clock 20000000\n");
                (void)sendServerCommand(&process, "baud 4000000\n");
                (void)sendServerMode(&process, false);
            }
            appendLog(&log, "SETTINGS  Restored application defaults");
        }
        if (restoreDefaults || bulkSizeChanged || ringSizeChanged ||
            dmaSizeChanged || clockChanged || baudChanged || modeChanged) {
            savedSettings = (AppDashboardSettings_t){
                .systemClockHz = (uint32_t)APP_DASHBOARD_SYSTEM_CLOCKS[clockIndex].size,
                .baud = (uint32_t)APP_DASHBOARD_BAUD_RATES[baudIndex].size,
                .ringBytes = (uint32_t)APP_DASHBOARD_RING_SIZES[ringSizeIndex].size,
                .dmaBytes = (uint32_t)APP_DASHBOARD_DMA_SIZES[dmaSizeIndex].size,
                .bulkBytes = (uint32_t)APP_DASHBOARD_BULK_SIZES[bulkSizeIndex].size,
                .byteIrq = state.byteIrqMode,
            };
            (void)AppClientPreferences_SaveSettings(
                APP_PREFERENCES_SERVER, &savedSettings);
        }
        EndDrawing();
    }
    (void)AppClientPreferences_SaveSettings(
        APP_PREFERENCES_SERVER, &savedSettings);

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
