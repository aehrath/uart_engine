/** @file
 * @brief Shared dashboard presets and selectable transfer/buffer sizes. */
#include "dashboard_config.h"

#include "h4_wire.h"

#include <stdio.h>

const AppDashboardPreset_t APP_DASHBOARD_PRESETS[] = {
    {"HCI Reset",           0x0C03U, {0U},             0U},
    {"Read Local Version",  0x1001U, {0U},             0U},
    {"Read BD_ADDR",        0x1009U, {0U},             0U},
    {"LE Read Buffer Size", 0x2002U, {0U},             0U},
    {"LE Enable Scan",      0x200CU, {0x01U, 0x00U},  2U},
    {"LE Disable Scan",     0x200CU, {0x00U, 0x00U},  2U},
};
const char *const APP_DASHBOARD_PRESET_OPTIONS[] = {
    "HCI Reset", "Read Local Version", "Read BD_ADDR",
    "LE Read Buffer Size", "LE Enable Scan", "LE Disable Scan",
};
const size_t APP_DASHBOARD_PRESET_COUNT =
    sizeof(APP_DASHBOARD_PRESETS) / sizeof(APP_DASHBOARD_PRESETS[0]);

const AppDashboardSizeChoice_t APP_DASHBOARD_BULK_SIZES[] = {
    {"16 KiB", 16U * 1024U}, {"64 KiB", 64U * 1024U},
    {"128 KiB", 128U * 1024U}, {"512 KiB", 512U * 1024U},
    {"1 MiB", 1U * 1024U * 1024U}, {"2 MiB", 2U * 1024U * 1024U},
    {"4 MiB", 4U * 1024U * 1024U}, {"8 MiB", 8U * 1024U * 1024U},
    {"16 MiB", 16U * 1024U * 1024U},
};
const char *const APP_DASHBOARD_BULK_SIZE_OPTIONS[] = {
    "16 KiB", "64 KiB", "128 KiB", "512 KiB", "1 MiB",
    "2 MiB", "4 MiB", "8 MiB", "16 MiB",
};
const size_t APP_DASHBOARD_BULK_SIZE_COUNT =
    sizeof(APP_DASHBOARD_BULK_SIZES) / sizeof(APP_DASHBOARD_BULK_SIZES[0]);

const AppDashboardSizeChoice_t APP_DASHBOARD_RING_SIZES[] = {
    {"256 B", 256U}, {"1 KiB", 1024U}, {"4 KiB", 4096U},
    {"16 KiB", 16U * 1024U}, {"64 KiB", 64U * 1024U},
};
const char *const APP_DASHBOARD_RING_SIZE_OPTIONS[] = {
    "256 B", "1 KiB", "4 KiB", "16 KiB", "64 KiB",
};
const size_t APP_DASHBOARD_RING_SIZE_COUNT =
    sizeof(APP_DASHBOARD_RING_SIZES) / sizeof(APP_DASHBOARD_RING_SIZES[0]);

const AppDashboardSizeChoice_t APP_DASHBOARD_DMA_SIZES[] = {
    {"32 B", 32U}, {"64 B", 64U}, {"128 B", 128U}, {"256 B", 256U},
    {"512 B", 512U}, {"1 KiB", 1024U}, {"4 KiB", 4096U},
};
const char *const APP_DASHBOARD_DMA_SIZE_OPTIONS[] = {
    "32 B", "64 B", "128 B", "256 B", "512 B", "1 KiB", "4 KiB",
};
const size_t APP_DASHBOARD_DMA_SIZE_COUNT =
    sizeof(APP_DASHBOARD_DMA_SIZES) / sizeof(APP_DASHBOARD_DMA_SIZES[0]);

void AppDashboardConfig_PresetToFields(size_t presetIndex,
                                       char *opcode, size_t opcodeSize,
                                       char *parameters,
                                       size_t parameterSize) {
    if (presetIndex >= APP_DASHBOARD_PRESET_COUNT) {
        return;
    }
    const AppDashboardPreset_t *preset = &APP_DASHBOARD_PRESETS[presetIndex];
    (void)snprintf(opcode, opcodeSize, "%04X", preset->opcode);
    AppH4_FormatHex(parameters, parameterSize,
                    preset->parameters, preset->parameterLength);
}
