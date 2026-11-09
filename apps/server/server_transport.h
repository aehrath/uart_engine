/** @file
 * @brief Local byte-stream transport used by the simulated BLE UART server. */
#ifndef APP_SERVER_TRANSPORT_H
#define APP_SERVER_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

/** Cross-platform local server endpoint. */
typedef struct {
#ifdef _WIN32
    HANDLE pipe;
    bool clientConnected;
#else
    int descriptor;
#endif
} AppServerTransport_t;

/** Create the local endpoint and return its user-facing name. */
bool AppServerTransport_Open(AppServerTransport_t *transport,
                             char *endpoint, size_t endpointCapacity);

/** Read currently available bytes without blocking the scheduler. */
int AppServerTransport_Read(AppServerTransport_t *transport,
                            uint8_t *data, size_t capacity);

/** Write bytes, returning zero when the transport should be retried. */
int AppServerTransport_Write(AppServerTransport_t *transport,
                             const uint8_t *data, size_t length);

/** Briefly yield until another write may make progress. */
void AppServerTransport_WaitWritable(AppServerTransport_t *transport,
                                     unsigned int milliseconds);

/** Release the endpoint and any connected local client. */
void AppServerTransport_Close(AppServerTransport_t *transport);

#endif
