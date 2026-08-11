/** @file
 * @brief Shared dashboard presets and selectable transfer/buffer sizes. */
#ifndef APP_DASHBOARD_CONFIG_H
#define APP_DASHBOARD_CONFIG_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

/** One editable HCI command preset. */
typedef struct {
    /** Human-readable picker label. */
    const char *name;
    /** Command opcode in host byte order. */
    uint16_t opcode;
    /** Fixed preset parameters. */
    uint8_t parameters[8];
    /** Number of valid parameter bytes. */
    size_t parameterLength;
} AppDashboardPreset_t;

/** One labeled byte-size choice. */
typedef struct {
    /** Human-readable dropdown label. */
    const char *label;
    /** Capacity or transfer length in bytes. */
    size_t size;
} AppDashboardSizeChoice_t;

extern const AppDashboardPreset_t APP_DASHBOARD_PRESETS[];
extern const char *const APP_DASHBOARD_PRESET_OPTIONS[];
extern const size_t APP_DASHBOARD_PRESET_COUNT;

extern const AppDashboardSizeChoice_t APP_DASHBOARD_BULK_SIZES[];
extern const char *const APP_DASHBOARD_BULK_SIZE_OPTIONS[];
extern const size_t APP_DASHBOARD_BULK_SIZE_COUNT;

extern const AppDashboardSizeChoice_t APP_DASHBOARD_RING_SIZES[];
extern const char *const APP_DASHBOARD_RING_SIZE_OPTIONS[];
extern const size_t APP_DASHBOARD_RING_SIZE_COUNT;

extern const AppDashboardSizeChoice_t APP_DASHBOARD_DMA_SIZES[];
extern const char *const APP_DASHBOARD_DMA_SIZE_OPTIONS[];
extern const size_t APP_DASHBOARD_DMA_SIZE_COUNT;

extern const AppDashboardSizeChoice_t APP_DASHBOARD_SYSTEM_CLOCKS[];
extern const char *const APP_DASHBOARD_SYSTEM_CLOCK_OPTIONS[];
extern const size_t APP_DASHBOARD_SYSTEM_CLOCK_COUNT;

extern const AppDashboardSizeChoice_t APP_DASHBOARD_BAUD_RATES[];
extern const char *const APP_DASHBOARD_BAUD_RATE_OPTIONS[];
extern const size_t APP_DASHBOARD_BAUD_RATE_COUNT;

/** Find a persisted numeric choice, falling back to a valid default index. */
int AppDashboardConfig_FindSize(const AppDashboardSizeChoice_t *choices,
                                size_t count, uint32_t value,
                                int defaultIndex);

/** Copy one preset into the dashboard's editable hexadecimal fields. */
void AppDashboardConfig_PresetToFields(size_t presetIndex,
                                       char *opcode, size_t opcodeSize,
                                       char *parameters,
                                       size_t parameterSize);

/** Parse min/max busy-cycle fields and a 0..100 workload weight. */
bool AppDashboardConfig_ParseThreadLoad(
    const char *minimumText, const char *maximumText, const char *weightText,
    uint32_t *minimum, uint32_t *maximum, uint32_t *weight);

#endif
