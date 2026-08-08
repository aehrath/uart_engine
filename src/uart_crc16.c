/** @file
 * @brief CRC16-CCITT implementation shared by RX validation and TX framing. */
#include "uart_dma/uart_crc16.h"

uint16_t UartDma_ComputeCrc16(const void *data, uint32_t length) {
    const uint8_t *const bytes = data;
    uint16_t crc = 0xFFFFU;
    for (uint32_t index = 0U; index < length; index++) {
        crc ^= (uint16_t)bytes[index] << 8;
        for (uint8_t bit = 0U; bit < 8U; bit++) {
            crc = (crc & 0x8000U) != 0U
                ? (uint16_t)((crc << 1) ^ 0x1021U)
                : (uint16_t)(crc << 1);
        }
    }
    return crc;
}
