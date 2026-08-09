/** @file
 * @brief Cross-platform persistent storage for host-client preferences. */
#include "client_preferences.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define APP_DIRECTORY_SEPARATOR '\\'
#else
#include <sys/stat.h>
#define APP_DIRECTORY_SEPARATOR '/'
#endif

#define APP_PREFERENCE_PATH_CAPACITY 1024U

/** Resolve the application-specific user configuration directory. */
static bool preferenceDirectory(char *output, size_t capacity) {
#ifdef _WIN32
    const char *const base = getenv("APPDATA");
    if (base == NULL || *base == '\0') return false;
    const int written = snprintf(output, capacity, "%s\\BLE UART", base);
    return written > 0 && (size_t)written < capacity;
#elif defined(__APPLE__)
    const char *const base = getenv("HOME");
    if (base == NULL || *base == '\0') return false;
    const int written = snprintf(
        output, capacity, "%s/Library/Application Support/BLE UART", base);
    return written > 0 && (size_t)written < capacity;
#else
    const char *base = getenv("XDG_CONFIG_HOME");
    char fallback[APP_PREFERENCE_PATH_CAPACITY];
    if (base == NULL || *base == '\0') {
        const char *const home = getenv("HOME");
        if (home == NULL || *home == '\0' ||
            snprintf(fallback, sizeof(fallback), "%s/.config", home) <= 0) {
            return false;
        }
        base = fallback;
    }
    const int written = snprintf(output, capacity, "%s/ble-uart", base);
    return written > 0 && (size_t)written < capacity;
#endif
}

/** Create one directory component while accepting an existing directory. */
static bool makeDirectory(const char *directory) {
#ifdef _WIN32
    return _mkdir(directory) == 0 || errno == EEXIST;
#else
    return mkdir(directory, 0700) == 0 || errno == EEXIST;
#endif
}

/** Recursively create the application preference directory. */
static bool ensurePreferenceDirectory(const char *directory) {
    char partial[APP_PREFERENCE_PATH_CAPACITY];
    const size_t length = strlen(directory);
    if (length == 0U || length >= sizeof(partial)) return false;
    memcpy(partial, directory, length + 1U);
    for (size_t index = 1U; index < length; index++) {
        const bool separator = partial[index] == '/' || partial[index] == '\\';
#ifdef _WIN32
        const bool drivePrefix = index == 2U && partial[1] == ':';
#else
        const bool drivePrefix = false;
#endif
        if (separator && !drivePrefix) {
            const char saved = partial[index];
            partial[index] = '\0';
            if (!makeDirectory(partial)) return false;
            partial[index] = saved;
        }
    }
    return makeDirectory(partial);
}

/** Resolve the port-preference filename and optionally create its directory. */
static bool preferencePath(char *output, size_t capacity, bool createDirectory,
                           const char *filename) {
    char directory[APP_PREFERENCE_PATH_CAPACITY];
    if (!preferenceDirectory(directory, sizeof(directory)) ||
        (createDirectory && !ensurePreferenceDirectory(directory))) {
        return false;
    }
    const int written = snprintf(output, capacity, "%s%c%s", directory,
                                 APP_DIRECTORY_SEPARATOR, filename);
    return written > 0 && (size_t)written < capacity;
}

bool AppClientPreferences_LoadPort(char *output, size_t capacity) {
    if (output == NULL || capacity < 2U) return false;
    char path[APP_PREFERENCE_PATH_CAPACITY];
    if (!preferencePath(path, sizeof(path), false, "client-port.txt")) return false;
    FILE *const file = fopen(path, "r");
    if (file == NULL) return false;
    const bool read = fgets(output, (int)capacity, file) != NULL;
    (void)fclose(file);
    if (!read) return false;
    output[strcspn(output, "\r\n")] = '\0';
    return output[0] != '\0';
}

bool AppClientPreferences_SavePort(const char *port) {
    if (port == NULL || *port == '\0' || strchr(port, '\n') != NULL ||
        strchr(port, '\r') != NULL) {
        return false;
    }
    char path[APP_PREFERENCE_PATH_CAPACITY];
    if (!preferencePath(path, sizeof(path), true, "client-port.txt")) return false;
    FILE *const file = fopen(path, "w");
    if (file == NULL) return false;
    const bool written = fprintf(file, "%s\n", port) > 0;
    return fclose(file) == 0 && written;
}

void AppClientPreferences_DefaultSettings(AppDashboardSettings_t *settings) {
    if (settings == NULL) return;
    *settings = (AppDashboardSettings_t){
        .systemClockHz = 20000000U,
        .baud = 4000000U,
        .ringBytes = 16U * 1024U,
        .dmaBytes = 256U,
        .bulkBytes = 2U * 1024U * 1024U,
        .byteIrq = false,
    };
}

/** Select the settings filename for one independent dashboard role. */
static const char *settingsFilename(AppPreferencesScope_t scope) {
    return scope == APP_PREFERENCES_SERVER ? "server-settings.txt"
                                           : "client-settings.txt";
}

bool AppClientPreferences_LoadSettings(AppPreferencesScope_t scope,
                                       AppDashboardSettings_t *settings) {
    if (settings == NULL) return false;
    AppClientPreferences_DefaultSettings(settings);
    char path[APP_PREFERENCE_PATH_CAPACITY];
    if (!preferencePath(path, sizeof(path), false, settingsFilename(scope))) return false;
    FILE *const file = fopen(path, "r");
    if (file == NULL) return false;
    AppDashboardSettings_t loaded = *settings;
    char line[128];
    while (fgets(line, sizeof(line), file) != NULL) {
        unsigned int value = 0U;
        if (sscanf(line, "clock=%u", &value) == 1) loaded.systemClockHz = value;
        else if (sscanf(line, "baud=%u", &value) == 1) loaded.baud = value;
        else if (sscanf(line, "ring=%u", &value) == 1) loaded.ringBytes = value;
        else if (sscanf(line, "dma=%u", &value) == 1) loaded.dmaBytes = value;
        else if (sscanf(line, "bulk=%u", &value) == 1) loaded.bulkBytes = value;
        else if (sscanf(line, "byte_irq=%u", &value) == 1) loaded.byteIrq = value != 0U;
    }
    (void)fclose(file);
    *settings = loaded;
    return true;
}

bool AppClientPreferences_SaveSettings(AppPreferencesScope_t scope,
                                       const AppDashboardSettings_t *settings) {
    if (settings == NULL) return false;
    char path[APP_PREFERENCE_PATH_CAPACITY];
    if (!preferencePath(path, sizeof(path), true, settingsFilename(scope))) return false;
    FILE *const file = fopen(path, "w");
    if (file == NULL) return false;
    const int written = fprintf(file,
        "clock=%u\nbaud=%u\nring=%u\ndma=%u\nbulk=%u\nbyte_irq=%u\n",
        settings->systemClockHz, settings->baud, settings->ringBytes,
        settings->dmaBytes, settings->bulkBytes, settings->byteIrq ? 1U : 0U);
    return fclose(file) == 0 && written > 0;
}
