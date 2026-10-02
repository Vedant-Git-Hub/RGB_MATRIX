/**
 * @file spi.c
 * @brief Cycle-timed WS2812B serial transmitter on Nano D13/PB5.
 */

#include "spi.h"

#include <avr/io.h>

#if F_CPU != 16000000UL
#error "The D13 WS2812B serial writer is tuned for a 16 MHz ATmega328P clock."
#endif

/**
 * @brief Emit one WS2812B-format byte on the D13 GPIO pin.
 * @param data Byte to send, MSB first.
 * @param high Complete PORTB value with PB5 high.
 * @param low Complete PORTB value with PB5 low.
 *
 * The inline assembly is deliberately cycle-counted: a zero bit is 20 CPU
 * cycles and a one bit is 19 CPU cycles. Its high pulses are 0.375 us and
 * 0.750 us respectively at the required 16 MHz CPU clock.
 */
static inline __attribute__((always_inline)) void spi_write_byte(uint8_t data,
                                                                   uint8_t high,
                                                                   uint8_t low)
{
    uint8_t bit_count = 8U;
    volatile uint8_t *const port = &PORTB;

    __asm__ volatile (
        "1:\n\t"
        "st X, %[high]\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "sbrs %[data], 7\n\t"
        "st X, %[low]\n\t"
        "lsl %[data]\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "st X, %[low]\n\t"
        "nop\n\t"
        "nop\n\t"
        "dec %[bit_count]\n\t"
        "brne 1b\n\t"
        : [data] "+r" (data), [bit_count] "+d" (bit_count)
        : "x" (port), [high] "r" (high), [low] "r" (low)
        : "memory"
    );
}

void spi_init(void)
{
    /** Configure D13/PB5 as a driven, idle-low output. */
    DDRB |= _BV(DDB5);
    PORTB &= (uint8_t)~_BV(PORTB5);
}

void spi_write(const uint8_t *data, uint16_t length)
{
    /** Preserve all other PORTB output bits while toggling PB5. */
    uint8_t low = PORTB;
    const uint8_t high = (uint8_t)(low | _BV(PORTB5));

    low &= (uint8_t)~_BV(PORTB5);

    while (length != 0U) {
        spi_write_byte(*data++, high, low);
        --length;
    }
}
