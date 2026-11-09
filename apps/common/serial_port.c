/** @file
 * @brief Platform implementations of the native serial-port abstraction. */
#include "serial_port.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <IOKit/serial/ioss.h>
#elif defined(__linux__)
/** Linux termios2 ABI used to request arbitrary supported baud rates. */
struct UartTermios2 {
    /** Input mode flags. */
    tcflag_t c_iflag;
    /** Output mode flags. */
    tcflag_t c_oflag;
    /** Control mode flags, including `BOTHER`. */
    tcflag_t c_cflag;
    /** Local mode flags. */
    tcflag_t c_lflag;
    /** Line discipline. */
    cc_t c_line;
    /** Control-character configuration. */
    cc_t c_cc[19];
    /** Requested input speed. */
    speed_t c_ispeed;
    /** Requested output speed. */
    speed_t c_ospeed;
};
#define UART_TCGETS2 _IOR('T', 0x2A, struct UartTermios2)
#define UART_TCSETS2 _IOW('T', 0x2B, struct UartTermios2)
#define UART_BOTHER  0010000U
#define UART_CBAUD   0010017U
#endif
#endif

void AppSerialPort_Init(AppSerialPort_t *port) {
    memset(port, 0, sizeof(*port));
#ifdef _WIN32
    port->handle = INVALID_HANDLE_VALUE;
#else
    port->descriptor = -1;
#endif
}

#ifdef _WIN32
bool AppSerialPort_Open(AppSerialPort_t *port, const char *name,
                        unsigned long baud, char *error, size_t error_size) {
    if (baud == 0UL || baud > 4000000UL) {
        (void)snprintf(error, error_size, "Baud must be between 1 and 4000000");
        return false;
    }
    const bool localSimulator = strcmp(name, "Local Simulator") == 0 ||
                                strcmp(name, "local://simulator") == 0;
    char device[128];
    if (localSimulator) {
        (void)snprintf(device, sizeof(device),
                       "\\\\.\\pipe\\ble-uart-local-simulator");
    } else {
        (void)snprintf(device, sizeof(device), "\\\\.\\%s", name);
    }
    port->handle = CreateFileA(device, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                               OPEN_EXISTING, 0, NULL);
    if (port->handle == INVALID_HANDLE_VALUE) {
        (void)snprintf(error, error_size, "Cannot open %s (Windows error %lu)",
                       name, (unsigned long)GetLastError());
        return false;
    }
    port->localSimulator = localSimulator;
    if (localSimulator) {
        port->connected = true;
        return true;
    }

    DCB settings = {0};
    settings.DCBlength = sizeof(settings);
    if (!GetCommState(port->handle, &settings)) {
        CloseHandle(port->handle);
        (void)snprintf(error, error_size, "Cannot read serial settings");
        return false;
    }
    settings.BaudRate = (DWORD)baud;
    settings.ByteSize = 8U;
    settings.StopBits = ONESTOPBIT;
    settings.Parity = NOPARITY;
    settings.fBinary = TRUE;
    settings.fDtrControl = DTR_CONTROL_ENABLE;
    settings.fRtsControl = RTS_CONTROL_ENABLE;

    COMMTIMEOUTS timeouts = {MAXDWORD, 0U, 0U, 0U, 0U};
    if (!SetCommState(port->handle, &settings) ||
        !SetCommTimeouts(port->handle, &timeouts)) {
        CloseHandle(port->handle);
        (void)snprintf(error, error_size, "Cannot configure serial port");
        return false;
    }
    PurgeComm(port->handle, PURGE_RXCLEAR | PURGE_TXCLEAR);
    port->connected = true;
    return true;
}

int AppSerialPort_Read(AppSerialPort_t *port, uint8_t *data, size_t capacity) {
    if (port->localSimulator) {
        DWORD available = 0U;
        if (!PeekNamedPipe(port->handle, NULL, 0U, NULL, &available, NULL)) {
            return -1;
        }
        if (available == 0U) return 0;
        if (capacity > available) capacity = available;
    }
    DWORD received = 0U;
    return ReadFile(port->handle, data, (DWORD)capacity, &received, NULL)
        ? (int)received : -1;
}

bool AppSerialPort_Write(AppSerialPort_t *port, const void *data, size_t length) {
    DWORD sent = 0U;
    return WriteFile(port->handle, data, (DWORD)length, &sent, NULL) &&
           sent == (DWORD)length;
}

void AppSerialPort_Close(AppSerialPort_t *port) {
    if (port->connected) {
        CloseHandle(port->handle);
    }
    AppSerialPort_Init(port);
}
#else
/** Map a standard numeric baud rate to a POSIX termios constant. */
static speed_t baudToSpeed(unsigned long baud) {
    switch (baud) {
        case 9600UL: return B9600;
        case 19200UL: return B19200;
        case 38400UL: return B38400;
        case 57600UL: return B57600;
        case 115200UL: return B115200;
#ifdef B230400
        case 230400UL: return B230400;
#endif
#ifdef B460800
        case 460800UL: return B460800;
#endif
#ifdef B921600
        case 921600UL: return B921600;
#endif
#ifdef B1000000
        case 1000000UL: return B1000000;
#endif
#ifdef B2000000
        case 2000000UL: return B2000000;
#endif
#ifdef B3000000
        case 3000000UL: return B3000000;
#endif
#ifdef B4000000
        case 4000000UL: return B4000000;
#endif
        default: return (speed_t)0;
    }
}

bool AppSerialPort_Open(AppSerialPort_t *port, const char *name,
                        unsigned long baud, char *error, size_t error_size) {
    if (baud == 0UL || baud > 4000000UL) {
        (void)snprintf(error, error_size, "Baud must be between 1 and 4000000");
        return false;
    }
    const speed_t speed = baudToSpeed(baud);
    const bool customBaud = speed == (speed_t)0;
    port->descriptor = open(name, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (port->descriptor < 0) {
        (void)snprintf(error, error_size, "Cannot open %s: %s", name, strerror(errno));
        return false;
    }

    struct termios settings;
    if (tcgetattr(port->descriptor, &settings) != 0) {
        (void)snprintf(error, error_size, "Cannot configure %s: %s", name, strerror(errno));
        close(port->descriptor);
        return false;
    }
    cfmakeraw(&settings);
    (void)cfsetispeed(&settings, customBaud ? B115200 : speed);
    (void)cfsetospeed(&settings, customBaud ? B115200 : speed);
    settings.c_cflag |= CLOCAL | CREAD;
    if (tcsetattr(port->descriptor, TCSANOW, &settings) != 0) {
        (void)snprintf(error, error_size, "Cannot configure %s: %s", name, strerror(errno));
        close(port->descriptor);
        return false;
    }

    if (customBaud) {
#if defined(__APPLE__)
        speed_t requestedSpeed = (speed_t)baud;
        if (ioctl(port->descriptor, IOSSIOSPEED, &requestedSpeed) != 0 &&
            strncmp(name, "/dev/ttys", 9U) != 0) {
            (void)snprintf(error, error_size, "Device rejected custom baud %lu: %s",
                           baud, strerror(errno));
            close(port->descriptor);
            return false;
        }
#elif defined(__linux__)
        struct UartTermios2 custom;
        if (ioctl(port->descriptor, UART_TCGETS2, &custom) != 0) {
            (void)snprintf(error, error_size, "Cannot request custom baud %lu: %s",
                           baud, strerror(errno));
            close(port->descriptor);
            return false;
        }
        custom.c_cflag = (custom.c_cflag & ~UART_CBAUD) | UART_BOTHER;
        custom.c_ispeed = (speed_t)baud;
        custom.c_ospeed = (speed_t)baud;
        if (ioctl(port->descriptor, UART_TCSETS2, &custom) != 0) {
            (void)snprintf(error, error_size, "Device rejected custom baud %lu: %s",
                           baud, strerror(errno));
            close(port->descriptor);
            return false;
        }
#else
        (void)snprintf(error, error_size, "Custom baud %lu is unsupported here", baud);
        close(port->descriptor);
        return false;
#endif
    }

    (void)tcflush(port->descriptor, TCIOFLUSH);
    port->connected = true;
    return true;
}

int AppSerialPort_Read(AppSerialPort_t *port, uint8_t *data, size_t capacity) {
    const ssize_t count = read(port->descriptor, data, capacity);
    if (count >= 0) {
        return (int)count;
    }
    return errno == EAGAIN || errno == EINTR ? 0 : -1;
}

bool AppSerialPort_Write(AppSerialPort_t *port, const void *data, size_t length) {
    const uint8_t *const bytes = data;
    size_t sent = 0U;
    while (sent < length) {
        const ssize_t count = write(port->descriptor, &bytes[sent], length - sent);
        if (count > 0) {
            sent += (size_t)count;
        } else if (errno != EAGAIN && errno != EINTR) {
            return false;
        }
    }
    return true;
}

void AppSerialPort_Close(AppSerialPort_t *port) {
    if (port->connected) {
        close(port->descriptor);
    }
    AppSerialPort_Init(port);
}

int AppSerialPort_Descriptor(const AppSerialPort_t *port) {
    return port->descriptor;
}
#endif
