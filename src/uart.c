/**
 * @file uart.c
 * @brief Interrupt-driven 115200-baud UART0 implementation.
 */

#include "uart.h"

#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/pgmspace.h>

#ifndef F_CPU
#error "F_CPU must be defined before compiling the UART driver."
#endif

#define UART_BAUD_RATE 115200UL
#define UART_UBRR_VALUE ((F_CPU / (8UL * UART_BAUD_RATE)) - 1UL)
#define UART_RX_BUFFER_SIZE 16U
#define UART_TX_BUFFER_SIZE 64U  /**< Queue capacity; menu output is flow-controlled. */

/** @brief Single-producer/single-consumer receive queue shared with the RX ISR. */
static volatile uint8_t rx_buffer[UART_RX_BUFFER_SIZE];
/** @brief Receive queue insertion index owned by the RX ISR. */
static volatile uint8_t rx_head;
/** @brief Receive queue removal index owned by the foreground loop. */
static volatile uint8_t rx_tail;
/** @brief Single-producer/single-consumer transmit queue drained by the UDRE ISR. */
static volatile uint8_t tx_buffer[UART_TX_BUFFER_SIZE];
/** @brief Transmit queue insertion index owned by the foreground loop. */
static volatile uint8_t tx_head;
/** @brief Transmit queue removal index owned by the UDRE ISR. */
static volatile uint8_t tx_tail;

_Static_assert((UART_RX_BUFFER_SIZE & (UART_RX_BUFFER_SIZE - 1U)) == 0U,
               "UART RX buffer size must be a power of two");
_Static_assert((UART_TX_BUFFER_SIZE & (UART_TX_BUFFER_SIZE - 1U)) == 0U,
               "UART TX buffer size must be a power of two");

ISR(USART_RX_vect)
{
    /** Copy hardware receive data into the RX ring unless it is full. */
    const uint8_t byte = UDR0;
    const uint8_t next_head = (rx_head + 1U) & (UART_RX_BUFFER_SIZE - 1U);

    if (next_head != rx_tail) {
        rx_buffer[rx_head] = byte;
        rx_head = next_head;
    }
}

ISR(USART_UDRE_vect)
{
    /** Move one queued byte to the UART hardware or disable this empty ISR. */
    if (tx_tail == tx_head) {
        UCSR0B &= (uint8_t)~_BV(UDRIE0);
        return;
    }

    UDR0 = tx_buffer[tx_tail];
    tx_tail = (tx_tail + 1U) & (UART_TX_BUFFER_SIZE - 1U);
}

void uart_init(void)
{
    /** Double-speed mode gives 117647 baud (2.1% error) at a 16 MHz clock. */
    UCSR0A = _BV(U2X0);
    UBRR0H = (uint8_t)(UART_UBRR_VALUE >> 8U);
    UBRR0L = (uint8_t)UART_UBRR_VALUE;
    rx_head = 0U;
    rx_tail = 0U;
    tx_head = 0U;
    tx_tail = 0U;
    UCSR0B = _BV(RXEN0) | _BV(TXEN0) | _BV(RXCIE0);
    UCSR0C = _BV(UCSZ01) | _BV(UCSZ00);
}

void uart_write_byte(uint8_t byte)
{
    const uint8_t next_head = (tx_head + 1U) & (UART_TX_BUFFER_SIZE - 1U);

    /** The UDRE ISR drains this queue in the background. */
    while (next_head == tx_tail) {
    }

    tx_buffer[tx_head] = byte;
    tx_head = next_head;
    UCSR0B |= _BV(UDRIE0);
}

void uart_write_flash(const char *flash_text)
{
    /** Read one program-memory byte at a time so strings do not occupy SRAM. */
    uint8_t character;

    while ((character = pgm_read_byte(flash_text++)) != 0U) {
        uart_write_byte(character);
    }
}

bool uart_read_byte(uint8_t *byte)
{
    /** The RX ISR owns rx_head; foreground code owns rx_tail. */
    if (rx_tail == rx_head) {
        return false;
    }

    *byte = rx_buffer[rx_tail];
    rx_tail = (rx_tail + 1U) & (UART_RX_BUFFER_SIZE - 1U);
    return true;
}
