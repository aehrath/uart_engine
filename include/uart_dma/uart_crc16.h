#ifndef UART_CRC16_H
#define UART_CRC16_H

/** @file
 * @brief CRC16-CCITT calculation for UART frames. */

#include <stdint.h>

/** Compute CRC16-CCITT using polynomial `0x1021` and initial value `0xffff`.
 * @param data Contiguous input bytes.
 * @param length Number of bytes to include.
 * @return The CRC in host byte order. */
uint16_t UartDma_ComputeCrc16(const void *data, uint32_t length);

#endif
