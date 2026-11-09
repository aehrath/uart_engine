/** @file
 * @brief POSIX PTY and Windows named-pipe local simulator transports. */
#include "server_transport.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32

#define APP_LOCAL_PIPE_NAME "\\\\.\\pipe\\ble-uart-local-simulator"

/** Complete a pending nonblocking named-pipe connection. */
static bool connectClient(AppServerTransport_t *transport) {
    if (transport->clientConnected) return true;
    if (ConnectNamedPipe(transport->pipe, NULL)) {
        transport->clientConnected = true;
        return true;
    }
    const DWORD error = GetLastError();
    if (error == ERROR_PIPE_CONNECTED) {
        transport->clientConnected = true;
        return true;
    }
    return false;
}

/** Reset the pipe after a client closes its endpoint. */
static void disconnectClient(AppServerTransport_t *transport) {
    (void)DisconnectNamedPipe(transport->pipe);
    transport->clientConnected = false;
}

bool AppServerTransport_Open(AppServerTransport_t *transport,
                             char *endpoint, size_t endpointCapacity) {
    memset(transport, 0, sizeof(*transport));
    transport->pipe = CreateNamedPipeA(
        APP_LOCAL_PIPE_NAME,
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT,
        1U, 64U * 1024U, 64U * 1024U, 0U, NULL);
    if (transport->pipe == INVALID_HANDLE_VALUE) return false;
    (void)snprintf(endpoint, endpointCapacity, "Local Simulator");
    return true;
}

int AppServerTransport_Read(AppServerTransport_t *transport,
                            uint8_t *data, size_t capacity) {
    if (!connectClient(transport)) return 0;
    DWORD available = 0U;
    if (!PeekNamedPipe(transport->pipe, NULL, 0U, NULL, &available, NULL)) {
        disconnectClient(transport);
        return 0;
    }
    if (available == 0U) return 0;
    const DWORD requested = capacity < available ? (DWORD)capacity : available;
    DWORD received = 0U;
    if (!ReadFile(transport->pipe, data, requested, &received, NULL)) {
        disconnectClient(transport);
        return 0;
    }
    return (int)received;
}

int AppServerTransport_Write(AppServerTransport_t *transport,
                             const uint8_t *data, size_t length) {
    if (!connectClient(transport)) return 0;
    const DWORD requested = length > MAXDWORD ? MAXDWORD : (DWORD)length;
    DWORD sent = 0U;
    if (!WriteFile(transport->pipe, data, requested, &sent, NULL)) {
        const DWORD error = GetLastError();
        if (error == ERROR_NO_DATA || error == ERROR_BROKEN_PIPE ||
            error == ERROR_PIPE_NOT_CONNECTED) {
            disconnectClient(transport);
            return 0;
        }
        return -1;
    }
    return (int)sent;
}

void AppServerTransport_Close(AppServerTransport_t *transport) {
    if (transport->pipe != NULL && transport->pipe != INVALID_HANDLE_VALUE) {
        if (transport->clientConnected) disconnectClient(transport);
        CloseHandle(transport->pipe);
    }
    transport->pipe = INVALID_HANDLE_VALUE;
    transport->clientConnected = false;
}

void AppServerTransport_WaitWritable(AppServerTransport_t *transport,
                                     unsigned int milliseconds) {
    (void)transport;
    Sleep(milliseconds);
}

#else

#include "pseudo_uart.h"
#include <poll.h>
#include <unistd.h>

bool AppServerTransport_Open(AppServerTransport_t *transport,
                             char *endpoint, size_t endpointCapacity) {
    transport->descriptor = PseudoUart_Open(endpoint, endpointCapacity);
    return transport->descriptor >= 0;
}

int AppServerTransport_Read(AppServerTransport_t *transport,
                            uint8_t *data, size_t capacity) {
    const ssize_t count = read(transport->descriptor, data, capacity);
    if (count >= 0) return (int)count;
    return errno == EAGAIN || errno == EINTR ? 0 : -1;
}

int AppServerTransport_Write(AppServerTransport_t *transport,
                             const uint8_t *data, size_t length) {
    const ssize_t count = write(transport->descriptor, data, length);
    if (count >= 0) return (int)count;
    return errno == EAGAIN || errno == EINTR ? 0 : -1;
}

void AppServerTransport_WaitWritable(AppServerTransport_t *transport,
                                     unsigned int milliseconds) {
    struct pollfd output = {transport->descriptor, POLLOUT, 0};
    (void)poll(&output, 1U, (int)milliseconds);
}

void AppServerTransport_Close(AppServerTransport_t *transport) {
    PseudoUart_Close(transport->descriptor);
    transport->descriptor = -1;
}

#endif
