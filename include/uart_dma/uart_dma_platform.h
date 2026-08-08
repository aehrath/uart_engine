#ifndef UART_DMA_PLATFORM_H
#define UART_DMA_PLATFORM_H

/** @file
 * @brief Minimal hardware abstraction required by the UART DMA core. */

#include <stdint.h>

/** UART register subset accessed by the portable stack. */
typedef struct {
    /** Interrupt/status register. */
    volatile uint32_t ISR;
    /** Interrupt-clear register. */
    volatile uint32_t ICR;
    /** Receive-data register placeholder. */
    volatile uint32_t RDR;
} UART_TypeDef;

/** DMA stream register subset accessed by the portable stack. */
typedef struct {
    /** Stream control register placeholder. */
    volatile uint32_t CR;
    /** Remaining transfer count used to derive the circular write cursor. */
    volatile uint32_t NDTR;
} DMA_Stream_TypeDef;

/** Platform UART instance supplied by the application or MCU port. */
extern UART_TypeDef *USART_HW;
/** Circular receive DMA stream supplied by the platform. */
extern DMA_Stream_TypeDef *DMA_RX_STREAM;
/** Transmit DMA stream supplied by the platform. */
extern DMA_Stream_TypeDef *DMA_TX_STREAM;

/** Start an asynchronous platform transmission.
 *
 * Override the weak default implementation for the target MCU or host transport.
 * The implementation must eventually invoke DMA_TX_Complete_IRQHandler().
 *
 * @param data Stable DMA source storage owned by the core.
 * @param size Number of bytes to transmit, including the CRC trailer. */
void UartDma_PlatformTransmitDma(uint8_t *data, uint16_t size);

#endif
