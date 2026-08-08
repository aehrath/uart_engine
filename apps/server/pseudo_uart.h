#ifndef BLE_SERVER_PSEUDO_UART_H
#define BLE_SERVER_PSEUDO_UART_H

/** @file
 * @brief POSIX pseudo-terminal transport used by the local BLE server. */

#include <stddef.h>

/** Create a raw, nonblocking pseudo-UART pair.
 * @param slave_name Receives the client-facing device path.
 * @param slave_name_size Capacity of @p slave_name.
 * @return Master descriptor, or `-1` on failure. */
int PseudoUart_Open(char *slave_name, size_t slave_name_size);
/** Close a pseudo-UART master descriptor when valid. */
void PseudoUart_Close(int descriptor);

#endif
