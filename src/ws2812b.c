/**
 * @file ws2812b.c
 * @brief Frame transmission and latch timing for the 256-pixel WS2812B matrix.
 */

#include "ws2812b.h"

#include <avr/interrupt.h>
#include <util/delay.h>

#include "spi.h"

#if F_CPU != 16000000UL
#error "The WS2812B SPI encoding is tuned for a 16 MHz ATmega328P clock."
#endif

void ws2812b_init(void)
{
    /** Ensure the one-wire output has been low for longer than a reset period. */
    spi_init();
    _delay_us(80);
}

void ws2812b_write(const ws2812b_rgb_t *pixels, uint16_t count)
{
    uint8_t saved_sreg;
    uint8_t zero = 0U;

    if (count > WS2812B_LED_COUNT) {
        count = WS2812B_LED_COUNT;
    }
    if (pixels == 0) {
        count = 0U;
    }

    /** An interrupt must not create a reset-length low pulse in the frame. */
    saved_sreg = SREG;
    cli();
    if (count != 0U) {
        /* ws2812b_rgb_t is stored in GRB order, so this is one continuous stream. */
        spi_write((const uint8_t *)pixels, (uint16_t)count * sizeof(ws2812b_rgb_t));
    }

    /** Pad a partial update with black so all 256 physical LEDs are refreshed. */
    while (count < WS2812B_LED_COUNT) {
        spi_write(&zero, 1U);
        ++count;
        spi_write(&zero, 1U);
        spi_write(&zero, 1U);
    }
    SREG = saved_sreg;

    /** WS2812B requires at least a 50 us low interval to latch this frame. */
    _delay_us(80);
}
