/**
 * @file uart.h
 * @brief Interrupt-buffered UART0 driver for the Nano USB serial interface.
 */

#ifndef UART_H
#define UART_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Initialise UART0 at 115200 baud, 8-N-1.
 *
 * UART0 uses Nano D0/RX and D1/TX. Receive and transmit are buffered by
 * USART interrupts; global interrupts must be enabled by the application.
 */
void uart_init(void);

/** @brief Queue one byte for interrupt-driven UART transmission.
 * @param byte Byte to append to the transmit queue.
 */
void uart_write_byte(uint8_t byte);

/**
 * @brief Queue a NUL-terminated string stored in AVR program flash.
 * @param flash_text Pointer created with PSTR().
 */
void uart_write_flash(const char *flash_text);

/**
 * @brief Fetch one queued received byte without blocking.
 * @param byte Destination for the received byte.
 * @return true when a byte was returned; false when the receive queue is empty.
 */
bool uart_read_byte(uint8_t *byte);

#endif
