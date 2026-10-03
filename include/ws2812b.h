/**
 * @file ws2812b.h
 * @brief Driver interface for a chain of 256 WS2812B RGB LEDs.
 */

#ifndef WS2812B_H
#define WS2812B_H

#include <stdint.h>

/** @brief Number of LEDs in the connected 16x16 matrix. */
#define WS2812B_LED_COUNT 256U

/**
 * @brief One LED colour in the WS2812B's native wire order.
 *
 * Field names retain normal RGB meaning, but storage order is GRB so a pixel
 * array can be transmitted without allocating a second conversion buffer.
 */
typedef struct {
    /* This physical order is intentional: WS2812B frames are GRB on the wire. */
    uint8_t green;
    uint8_t red;
    uint8_t blue;
} ws2812b_rgb_t;

_Static_assert(sizeof(ws2812b_rgb_t) == 3U, "WS2812B pixels must be tightly packed");

/** @brief Initialise the D13 WS2812B output and leave it idle low. */
void ws2812b_init(void);

/**
 * @brief Set and persist the global brightness level.
 * @param brightness Brightness level from 0 (off) to 15 (full).
 */
void ws2812b_set_brightness(uint8_t brightness);

/** @brief Return the current persisted global brightness level. */
uint8_t ws2812b_get_brightness(void);

/**
 * @brief Refresh the physical LED chain using the configured brightness.
 * @param pixels Pixel array in physical DIN-to-DOUT chain order, or NULL for black.
 * @param count Number of supplied pixels; values above 256 are clamped.
 *
 * Any unsupplied LEDs are transmitted as black. The function masks interrupts
 * only for the approximately 7.5 ms timing-sensitive data waveform.
 */
void ws2812b_write(const ws2812b_rgb_t *pixels, uint16_t count);


#endif
