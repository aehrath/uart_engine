/** @file
 * @brief Shared real-time producer/consumer pipeline monitor. */
#include "dashboard_pipeline.h"

#include "dashboard_ui.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

void AppDashboardPipeline_Reset(AppDashboardPipeline_t *pipeline) {
    if (pipeline != NULL) {
        memset(pipeline, 0, sizeof(*pipeline));
        pipeline->headroomUs = UINT64_MAX;
    }
}

/** Convert an interval high-water mark to a chart-safe capacity percentage. */
static float capacityPercent(uint32_t highWater, uint32_t capacity) {
    if (capacity == 0U) return 0.0f;
    const float percentage = 100.0f * (float)highWater / (float)capacity;
    return percentage > 100.0f ? 100.0f : percentage;
}

void AppDashboardPipeline_Sample(AppDashboardPipeline_t *pipeline) {
    if (pipeline == NULL) return;
    pipeline->producerHistory[pipeline->historyHead] = (float)pipeline->producerBps;
    pipeline->consumerHistory[pipeline->historyHead] = (float)pipeline->consumerBps;
    pipeline->queueHistory[pipeline->historyHead] = capacityPercent(
        pipeline->queueHighWater, pipeline->queueCapacity);
    pipeline->ringHistory[pipeline->historyHead] = capacityPercent(
        pipeline->ringHighWaterBytes, pipeline->ringCapacity);
    pipeline->dmaHistory[pipeline->historyHead] = capacityPercent(
        pipeline->dmaHighWaterBytes, pipeline->dmaCapacity);
    /* Hold the interval peak for one display interval. This makes short DMA
     * bursts visible without leaving stale occupancy after traffic stops. */
    pipeline->queueDepth = pipeline->queueHighWater;
    pipeline->ringBurstBytes = pipeline->ringHighWaterBytes;
    pipeline->dmaBurstBytes = pipeline->dmaHighWaterBytes;
    pipeline->historyHead = (pipeline->historyHead + 1U) % APP_DASHBOARD_PIPELINE_HISTORY;
    if (pipeline->historyCount < APP_DASHBOARD_PIPELINE_HISTORY) pipeline->historyCount++;
    pipeline->queueHighWater = 0U;
    pipeline->ringHighWaterBytes = 0U;
    pipeline->dmaHighWaterBytes = 0U;
}

/** Format a byte rate compactly. */
static void formatRate(char *output, size_t capacity, uint64_t rate) {
    if (rate >= 1000000U) (void)snprintf(output, capacity, "%.2f MB/s", (double)rate / 1e6);
    else if (rate >= 1000U) (void)snprintf(output, capacity, "%.1f kB/s", (double)rate / 1e3);
    else (void)snprintf(output, capacity, "%" PRIu64 " B/s", rate);
}

/** Format latency or time-to-overrun. */
static void formatDuration(char *output, size_t capacity, uint64_t us, bool stable) {
    if (stable && us == UINT64_MAX) (void)snprintf(output, capacity, "stable");
    else if (us >= 1000000U) (void)snprintf(output, capacity, "%.2f s", (double)us / 1e6);
    else if (us >= 1000U) (void)snprintf(output, capacity, "%.1f ms", (double)us / 1e3);
    else (void)snprintf(output, capacity, "%" PRIu64 " us", us);
}

/** Draw one history series. */
static void drawLine(const AppDashboardPipeline_t *p, const float *history,
                     Rectangle bounds, float maximum, float thickness,
                     Color color) {
    if (p->historyCount == 0U || maximum <= 0.0f) return;
    const size_t oldest = (p->historyHead + APP_DASHBOARD_PIPELINE_HISTORY -
                           p->historyCount) % APP_DASHBOARD_PIPELINE_HISTORY;
    for (size_t sample = 1U; sample < p->historyCount; ++sample) {
        const size_t a = (oldest + sample - 1U) % APP_DASHBOARD_PIPELINE_HISTORY;
        const size_t b = (oldest + sample) % APP_DASHBOARD_PIPELINE_HISTORY;
        const float x0 = bounds.x + bounds.width * (float)(sample - 1U) /
            (float)(APP_DASHBOARD_PIPELINE_HISTORY - 1U);
        const float x1 = bounds.x + bounds.width * (float)sample /
            (float)(APP_DASHBOARD_PIPELINE_HISTORY - 1U);
        DrawLineEx((Vector2){x0, bounds.y + bounds.height - bounds.height * history[a] / maximum},
                   (Vector2){x1, bounds.y + bounds.height - bounds.height * history[b] / maximum},
                   thickness, color);
    }
    const size_t newest = (p->historyHead + APP_DASHBOARD_PIPELINE_HISTORY - 1U) %
        APP_DASHBOARD_PIPELINE_HISTORY;
    const float newestX = bounds.x + bounds.width * (float)(p->historyCount - 1U) /
        (float)(APP_DASHBOARD_PIPELINE_HISTORY - 1U);
    const float newestY = bounds.y + bounds.height -
        bounds.height * history[newest] / maximum;
    DrawCircleV((Vector2){newestX, newestY}, 2.5f, color);
}

/** Return the largest percentage retained by a circular history. */
static float retainedPeakPercent(const float *history, size_t count,
                                 size_t head) {
    float peak = 0.0f;
    const size_t oldest = (head + APP_DASHBOARD_PIPELINE_HISTORY - count) %
        APP_DASHBOARD_PIPELINE_HISTORY;
    for (size_t sample = 0U; sample < count; sample++) {
        const float value = history[(oldest + sample) %
            APP_DASHBOARD_PIPELINE_HISTORY];
        if (value > peak) peak = value;
    }
    return peak;
}

/** Draw one retained percentage gauge. */
static void drawGauge(const char *label, float percentage,
                      Rectangle bounds, Color color) {
    if (percentage < 0.0f) percentage = 0.0f;
    if (percentage > 100.0f) percentage = 100.0f;
    char value[64];
    (void)snprintf(value, sizeof(value), "%s %.1f%%", label,
                   (double)percentage);
    AppDashboardUi_DrawText(value, (int)bounds.x, (int)bounds.y - 2, 10, COLOR_MUTED);
    const Rectangle track = {bounds.x + 94.0f, bounds.y, bounds.width - 94.0f, bounds.height};
    DrawRectangleRounded(track, 1.0f, 8, (Color){35, 45, 65, 255});
    if (percentage > 0.0f) {
        Rectangle fill = track;
        fill.width *= percentage / 100.0f;
        DrawRectangleRounded(fill, 1.0f, 8, color);
    }
}

void AppDashboardPipeline_Draw(const AppDashboardPipeline_t *p, Rectangle bounds) {
    if (p == NULL) return;
    DrawRectangleRounded(bounds, 16.0f / bounds.height, 12, (Color){7, 11, 20, 255});
    char producer[32], consumer[32], legend[96];
    formatRate(producer, sizeof(producer), p->producerBps);
    formatRate(consumer, sizeof(consumer), p->consumerBps);
    (void)snprintf(legend, sizeof(legend), "PRODUCER  %s     CONSUMER  %s", producer, consumer);
    AppDashboardUi_DrawDisplayText(legend, (int)bounds.x + 12, (int)bounds.y + 8, 10, COLOR_TEXT);
    const Rectangle graph = {bounds.x + 12.0f, bounds.y + 28.0f, bounds.width - 24.0f, 43.0f};
    DrawRectangleRounded(graph, 0.15f, 8, (Color){12, 19, 33, 255});
    AppDashboardUi_DrawText("Q%", (int)(graph.x + graph.width - 66),
                            (int)graph.y + 2, 9, COLOR_PURPLE);
    AppDashboardUi_DrawText("R%", (int)(graph.x + graph.width - 44),
                            (int)graph.y + 2, 9, (Color){255, 196, 92, 255});
    AppDashboardUi_DrawText("D%", (int)(graph.x + graph.width - 22),
                            (int)graph.y + 2, 9, (Color){145, 152, 166, 255});
    DrawLine((int)graph.x, (int)(graph.y + graph.height / 2.0f),
             (int)(graph.x + graph.width), (int)(graph.y + graph.height / 2.0f), COLOR_BORDER);
    float maximum = 1.0f;
    for (size_t i = 0U; i < p->historyCount; ++i) {
        const size_t n = (p->historyHead + APP_DASHBOARD_PIPELINE_HISTORY - p->historyCount + i) %
            APP_DASHBOARD_PIPELINE_HISTORY;
        if (p->producerHistory[n] > maximum) maximum = p->producerHistory[n];
        if (p->consumerHistory[n] > maximum) maximum = p->consumerHistory[n];
    }
    drawLine(p, p->producerHistory, graph, maximum, 2.0f, COLOR_BLUE);
    drawLine(p, p->consumerHistory, graph, maximum, 2.0f, COLOR_CYAN);
    /* Draw the wider DMA trace first so an equal ring percentage remains
     * visible as an amber centerline instead of being hidden by gray. */
    drawLine(p, p->dmaHistory, graph, 100.0f, 4.0f,
             (Color){145, 152, 166, 255});
    drawLine(p, p->ringHistory, graph, 100.0f, 2.0f,
             (Color){255, 196, 92, 255});
    drawLine(p, p->queueHistory, graph, 100.0f, 2.0f, COLOR_PURPLE);
    float queuePeak = retainedPeakPercent(
        p->queueHistory, p->historyCount, p->historyHead);
    float ringPeak = retainedPeakPercent(
        p->ringHistory, p->historyCount, p->historyHead);
    const float liveQueuePeak = capacityPercent(
        p->queueHighWater > p->queueDepth ? p->queueHighWater : p->queueDepth,
        p->queueCapacity);
    const float liveRingPeak = capacityPercent(
        p->ringHighWaterBytes > p->ringBurstBytes
            ? p->ringHighWaterBytes : p->ringBurstBytes,
        p->ringCapacity);
    if (liveQueuePeak > queuePeak) queuePeak = liveQueuePeak;
    if (liveRingPeak > ringPeak) ringPeak = liveRingPeak;
    drawGauge("Q PEAK", queuePeak,
              (Rectangle){bounds.x + 12, bounds.y + 78, bounds.width - 24, 8},
              COLOR_PURPLE);
    drawGauge("R PEAK", ringPeak,
              (Rectangle){bounds.x + 12, bounds.y + 95, bounds.width - 24, 8},
              COLOR_BLUE);
    char latency[24], headroom[24], footer[128];
    formatDuration(latency, sizeof(latency), p->latencyUs, false);
    formatDuration(headroom, sizeof(headroom), p->headroomUs, true);
    (void)snprintf(footer, sizeof(footer), "LATENCY %s   HEADROOM %s   DROPS %u/%u/%u",
                   latency, headroom, p->crcDrops, p->queueDrops, p->ringDrops);
    const Color color = p->headroomUs != UINT64_MAX && p->headroomUs < 2000000U
        ? (Color){255, 120, 120, 255}
        : p->headroomUs != UINT64_MAX && p->headroomUs < 10000000U
            ? (Color){255, 196, 92, 255} : COLOR_MUTED;
    AppDashboardUi_DrawText(footer, (int)bounds.x + 12, (int)bounds.y + 114, 10, color);
}
