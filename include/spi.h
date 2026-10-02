/**
 * @file spi.h
 * @brief Timing-critical, transmit-only data interface on Arduino Nano D13.
 */

#ifndef SPI_H
#define SPI_H

#include <stdint.h>

/**
 * @brief Configure PB5/D13 as the idle-low WS2812B data output.
 *
 * PB5 is normally the hardware SPI SCK pin. The matrix DIN is connected to
 * D13, so this project deliberately uses a cycle-timed GPIO implementation
 * rather than hardware SPI.
 */
void spi_init(void);

/**
 * @brief Send a raw byte sequence, most-significant bit first, on D13.
 * @param data Pointer to bytes in SRAM.
 * @param length Number of bytes to transmit.
 *
 * The caller is responsible for ensuring the byte sequence is a valid
 * WS2812B data stream and that interrupts cannot disturb it.
 */
void spi_write(const uint8_t *data, uint16_t length);

#endif
