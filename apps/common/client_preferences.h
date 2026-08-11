#ifndef APP_CLIENT_PREFERENCES_H
#define APP_CLIENT_PREFERENCES_H

/** @file
 * @brief Persistent host-client preferences stored in the user config folder. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Independent settings owner. */
typedef enum {
    APP_PREFERENCES_CLIENT,
    APP_PREFERENCES_SERVER,
} AppPreferencesScope_t;

/** Persisted operating controls shared by all dashboards. */
typedef struct {
    /** LPUART peripheral source clock in hertz. */
    uint32_t systemClockHz;
    /** Requested UART line rate. */
    uint32_t baud;
    /** Circular receive-ring capacity. */
    uint32_t ringBytes;
    /** Maximum bytes completed by one DMA transaction. */
    uint32_t dmaBytes;
    /** Selected deterministic large-transfer size. */
    uint32_t bulkBytes;
    /** Whether the local receiver uses one IRQ per byte. */
    bool byteIrq;
    /** Percentage weight applied to the random min/max range. */
    uint32_t threadLoadWeightPercent;
    /** Busy-cycle range; a zero maximum disables the workload. */
    uint32_t threadLoadMinimumCycles;
    uint32_t threadLoadMaximumCycles;
} AppDashboardSettings_t;

/** Populate the documented application defaults. */
void AppClientPreferences_DefaultSettings(AppDashboardSettings_t *settings);
/** Load saved settings for one independent app scope. */
bool AppClientPreferences_LoadSettings(AppPreferencesScope_t scope,
                                       AppDashboardSettings_t *settings);
/** Save settings for one independent app scope. */
bool AppClientPreferences_SaveSettings(AppPreferencesScope_t scope,
                                       const AppDashboardSettings_t *settings);

/** Load the most recently selected UART port.
 * @param output Destination text buffer.
 * @param capacity Size of @p output in bytes.
 * @return `true` when a nonempty saved port was loaded. */
bool AppClientPreferences_LoadPort(char *output, size_t capacity);

/** Save the currently selected UART port.
 * @param port Nonempty platform UART path or device name.
 * @return `true` when the preference was written successfully. */
bool AppClientPreferences_SavePort(const char *port);

#endif
