#ifndef APP_SERIAL_PORT_H
#define APP_SERIAL_PORT_H

/** @file
 * @brief Portable Windows, macOS, and Linux serial-port abstraction.
 * @defgroup app_transport Host transport utilities
 * @brief Serial-port access and H4 framing shared by the native clients.
 * @{ */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Open serial-port state with a platform-native handle. */
typedef struct {
#ifdef _WIN32
    /** Opaque Windows communications or named-pipe handle. */
    void *handle;
    /** Whether the handle is the driver-free local simulator named pipe. */
    bool localSimulator;
#else
    /** POSIX file descriptor. */
    int descriptor;
#endif
    /** Whether the native resource is currently open and configured. */
    bool connected;
} AppSerialPort_t;

/** Initialize a closed port object. @param port Object to initialize. */
void AppSerialPort_Init(AppSerialPort_t *port);
/** Open and configure a serial device in raw 8-N-1 mode.
 * @param port Initialized port object.
 * @param name Device path or Windows COM name.
 * @param baud Requested rate from 1 through 4,000,000 baud.
 * @param error Destination for a human-readable failure description.
 * @param error_size Capacity of @p error.
 * @return `true` on success. */
bool AppSerialPort_Open(AppSerialPort_t *port, const char *name,
                        unsigned long baud, char *error, size_t error_size);
/** Perform a nonblocking serial read.
 * @return Positive byte count, zero when no data is ready, or `-1` on error. */
int AppSerialPort_Read(AppSerialPort_t *port, uint8_t *data, size_t capacity);
/** Write an entire typed packet or byte span.
 * @return `true` only when all @p length bytes were written. */
bool AppSerialPort_Write(AppSerialPort_t *port, const void *data, size_t length);
/** Close the native resource and reset the object. @param port Port to close. */
void AppSerialPort_Close(AppSerialPort_t *port);

#ifndef _WIN32
/** Get the POSIX descriptor for polling. @return Open descriptor. */
int AppSerialPort_Descriptor(const AppSerialPort_t *port);
#endif

/** @} */

#endif
