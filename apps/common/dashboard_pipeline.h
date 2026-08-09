/** @file
 * @brief Shared real-time producer/consumer pipeline monitor. */
#ifndef APP_DASHBOARD_PIPELINE_H
#define APP_DASHBOARD_PIPELINE_H

#include "raylib.h"

#include <stddef.h>
#include <stdint.h>

#define APP_DASHBOARD_PIPELINE_HISTORY 120U

/** Display-ready snapshot and one-minute circular rate history. */
typedef struct {
    uint64_t producerBps;
    uint64_t consumerBps;
    uint32_t queuedBytes;
    uint32_t queueDepth;
    uint32_t queueCapacity;
    uint32_t queueHighWater;
    uint32_t ringBurstBytes;
    uint32_t ringCapacity;
    uint32_t ringHighWaterBytes;
    uint32_t dmaBurstBytes;
    uint32_t dmaCapacity;
    uint32_t dmaHighWaterBytes;
    uint64_t latencyUs;
    uint64_t latencyMaxUs;
    uint64_t headroomUs;
    uint32_t crcDrops;
    uint32_t queueDrops;
    uint32_t ringDrops;
    float producerHistory[APP_DASHBOARD_PIPELINE_HISTORY];
    float consumerHistory[APP_DASHBOARD_PIPELINE_HISTORY];
    float queueHistory[APP_DASHBOARD_PIPELINE_HISTORY];
    float ringHistory[APP_DASHBOARD_PIPELINE_HISTORY];
    float dmaHistory[APP_DASHBOARD_PIPELINE_HISTORY];
    size_t historyCount;
    size_t historyHead;
} AppDashboardPipeline_t;

/** Clear all current measurements and retained history. */
void AppDashboardPipeline_Reset(AppDashboardPipeline_t *pipeline);

/** Append the current rates to chronological circular history. */
void AppDashboardPipeline_Sample(AppDashboardPipeline_t *pipeline);

/** Draw the standard rate graph, pressure gauges, and headroom footer. */
void AppDashboardPipeline_Draw(const AppDashboardPipeline_t *pipeline,
                               Rectangle bounds);

#endif
