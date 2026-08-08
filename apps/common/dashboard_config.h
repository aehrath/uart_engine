/** @file
 * @brief Shared dashboard presets and selectable transfer/buffer sizes. */
#ifndef APP_DASHBOARD_CONFIG_H
#define APP_DASHBOARD_CONFIG_H

#include <stddef.h>
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

/** Copy one preset into the dashboard's editable hexadecimal fields. */
void AppDashboardConfig_PresetToFields(size_t presetIndex,
                                       char *opcode, size_t opcodeSize,
                                       char *parameters,
                                       size_t parameterSize);

#endif
