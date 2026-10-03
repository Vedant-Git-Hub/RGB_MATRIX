/**
 * @file ws2812b.c
 * @brief Frame transmission and latch timing for the 256-pixel WS2812B matrix.
 */

#include "ws2812b.h"

#include <avr/eeprom.h>
#include <avr/interrupt.h>
#include <util/delay.h>

#include "spi.h"

#if F_CPU != 16000000UL
#error "The WS2812B SPI encoding is tuned for a 16 MHz ATmega328P clock."
#endif

/** @brief Full intensity used when EEPROM has not been initialized. */
#define WS2812B_DEFAULT_BRIGHTNESS 15U

/** @brief EEPROM slot containing the persisted 0-15 global brightness. */
static uint8_t stored_brightness EEMEM = WS2812B_DEFAULT_BRIGHTNESS;
/** @brief Brightness applied to every transmitted frame. */
static uint8_t brightness_level = WS2812B_DEFAULT_BRIGHTNESS;

void ws2812b_init(void)
{
    const uint8_t saved_brightness = eeprom_read_byte(&stored_brightness);

    /** Ensure the one-wire output has been low for longer than a reset period. */
    spi_init();
    if (saved_brightness <= 15U) {
        brightness_level = saved_brightness;
    } else {
        brightness_level = WS2812B_DEFAULT_BRIGHTNESS;
        eeprom_update_byte(&stored_brightness, brightness_level);
    }
    _delay_us(80);
}

/** @brief Set and persist the driver-wide brightness level. */
void ws2812b_set_brightness(uint8_t brightness)
{
    if (brightness > 15U) {
        brightness = 15U;
    }
    brightness_level = brightness;
    eeprom_update_byte(&stored_brightness, brightness_level);
}

/** @brief Return the driver-wide brightness level. */
uint8_t ws2812b_get_brightness(void)
{
    return brightness_level;
}

/** @brief Refresh the chain using the driver's persisted brightness level. */
void ws2812b_write(const ws2812b_rgb_t *pixels, uint16_t count)
{
    uint8_t saved_sreg;
    uint8_t zero = 0U;
    uint16_t sent = 0U;

    if (count > WS2812B_LED_COUNT) {
        count = WS2812B_LED_COUNT;
    }
    if (pixels == 0) {
        count = 0U;
    }

    /** An interrupt must not create a reset-length low pulse in the frame. */
    saved_sreg = SREG;
    cli();
    if (count != 0U && brightness_level >= 15U) {
        /* ws2812b_rgb_t is stored in GRB order, so this is one continuous stream. */
        spi_write((const uint8_t *)pixels, (uint16_t)count * sizeof(ws2812b_rgb_t));
        sent = count;
    } else if (count != 0U && brightness_level != 0U) {
        /* Scale one pixel at a time so the framebuffer is never modified. */
        while (sent < count) {
            ws2812b_rgb_t scaled = pixels[sent];

            scaled.green = (uint8_t)((uint16_t)scaled.green * brightness_level / 15U);
            scaled.red = (uint8_t)((uint16_t)scaled.red * brightness_level / 15U);
            scaled.blue = (uint8_t)((uint16_t)scaled.blue * brightness_level / 15U);
            spi_write((const uint8_t *)&scaled, sizeof(scaled));
            ++sent;
        }
    }

    /** Pad a partial update with black so all 256 physical LEDs are refreshed. */
    while (sent < WS2812B_LED_COUNT) {
        spi_write(&zero, 1U);
        ++sent;
        spi_write(&zero, 1U);
        spi_write(&zero, 1U);
    }
    SREG = saved_sreg;

    /** WS2812B requires at least a 50 us low interval to latch this frame. */
    _delay_us(80);
}
