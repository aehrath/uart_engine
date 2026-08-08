/** @file
 * @brief POSIX pseudo-terminal creation for the local server. */
#include "pseudo_uart.h"

#include <fcntl.h>
#include <stdio.h>
#include <termios.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

int PseudoUart_Open(char *slave_name, size_t slave_name_size) {
    int master = -1;
    int slave = -1;
    char nameBuffer[128] = {0};
    if (openpty(&master, &slave, nameBuffer, NULL, NULL) != 0) {
        return -1;
    }

    struct termios settings;
    if (tcgetattr(slave, &settings) == 0) {
        cfmakeraw(&settings);
        (void)tcsetattr(slave, TCSANOW, &settings);
    }
    (void)fcntl(master, F_SETFL, fcntl(master, F_GETFL) | O_NONBLOCK);
    (void)snprintf(slave_name, slave_name_size, "%s", nameBuffer);
    close(slave);
    return master;
}

void PseudoUart_Close(int descriptor) {
    if (descriptor >= 0) {
        close(descriptor);
    }
}
