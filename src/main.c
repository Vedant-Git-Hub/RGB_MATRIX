/**
 * @file main.c
 * @brief Animation state machine and UART-controlled 8x8 matrix application.
 *
 * The foreground loop drains UART events, applies the selected animation to a
 * logical framebuffer, and refreshes the WS2812B chain every 100 ms.
 */

#include <stdbool.h>
#include <stdint.h>

#include <avr/interrupt.h>
#include <avr/pgmspace.h>
#include <util/delay.h>

#include "uart.h"
#include "ws2812b.h"

#define MATRIX_WIDTH  8U
#define MATRIX_HEIGHT 8U

/* Set to 0 when the LEDs are wired left-to-right on every row. */
#define MATRIX_ROWS_ARE_SERPENTINE 1U
#define ANIMATION_FRAME_DELAY_MS 100U

/** @brief Top-level application states. */
typedef enum {
    APP_SHOW_MENU,
    APP_RUNNING_ANIMATION
} app_state_t;

/** @brief UART-selectable animation identifiers. */
typedef enum {
    ANIMATION_RAIN,
    ANIMATION_RAINBOW_WAVE,
    ANIMATION_COMET,
    ANIMATION_STARFIELD,
    ANIMATION_COLOUR_WIPE,
    ANIMATION_THEATER_CHASE,
    ANIMATION_SCANNER,
    ANIMATION_PLASMA,
    ANIMATION_3D_CUBE,
    ANIMATION_3D_TUNNEL
} animation_t;

/* The palette resides in program flash, never consuming SRAM. */
static const ws2812b_rgb_t colour_palette[] PROGMEM = {
    {.green = 24U,  .red = 255U, .blue = 0U},
    {.green = 180U, .red = 255U, .blue = 0U},
    {.green = 255U, .red = 0U,   .blue = 32U},
    {.green = 180U, .red = 0U,   .blue = 255U},
    {.green = 64U,  .red = 32U,  .blue = 255U},
    {.green = 0U,   .red = 220U, .blue = 255U}
};

#define COLOUR_PALETTE_COUNT (sizeof(colour_palette) / sizeof(colour_palette[0]))

/*
 * Static cube vertices packed as 0xXY, where X is the column and Y is the
 * row. Vertices 0-3 form the rear face and 4-7 form the front face.
 */
static const uint8_t cube_vertices[8] PROGMEM = {
    0x20U, 0x50U, 0x52U, 0x22U,
    0x04U, 0x34U, 0x37U, 0x07U
};

/* Four edges per face, then the four edges that convey depth. */
static const uint8_t cube_edges[][2] PROGMEM = {
    {0U, 1U}, {1U, 2U}, {2U, 3U}, {3U, 0U},
    {4U, 5U}, {5U, 6U}, {6U, 7U}, {7U, 4U},
    {0U, 4U}, {1U, 5U}, {2U, 6U}, {3U, 7U}
};

/** @brief Logical 8x8 framebuffer stored in native WS2812B GRB order. */
static ws2812b_rgb_t matrix_pixels[WS2812B_LED_COUNT];
static uint16_t random_state = 0xC0DEU;
static int8_t rain_row[MATRIX_WIDTH];
static ws2812b_rgb_t rain_colour[MATRIX_WIDTH];
static uint8_t rainbow_phase;
static uint8_t comet_position;
static ws2812b_rgb_t comet_colour;
static uint8_t wipe_position;
static ws2812b_rgb_t wipe_colour;
static uint8_t chase_phase;
static int8_t scanner_row;
static int8_t scanner_direction;
static uint8_t plasma_phase;
static uint8_t tunnel_phase;

/** @brief Return the next byte from the deterministic 16-bit LFSR. */
static uint8_t random_u8(void)
{
    const uint16_t low_bit = random_state & 1U;

    random_state >>= 1U;
    if (low_bit != 0U) {
        random_state ^= 0xB400U;
    }

    return (uint8_t)random_state;
}

/** @brief Choose one saturated colour from the flash-resident palette. */
static ws2812b_rgb_t random_colour(void)
{
    const uint8_t index = random_u8() % COLOUR_PALETTE_COUNT;
    ws2812b_rgb_t colour;

    colour.green = pgm_read_byte(&colour_palette[index].green);
    colour.red = pgm_read_byte(&colour_palette[index].red);
    colour.blue = pgm_read_byte(&colour_palette[index].blue);
    return colour;
}

/**
 * @brief Set one logical pixel while applying the panel's wiring map.
 * @param row Logical row, 0 through 7.
 * @param column Logical column, 0 through 7.
 * @param colour GRB colour to write.
 */
static void matrix_set_pixel(uint8_t row, uint8_t column, ws2812b_rgb_t colour)
{
    uint8_t index;

#if MATRIX_ROWS_ARE_SERPENTINE
    if ((row & 1U) == 0U) {
        index = (uint8_t)(row * MATRIX_WIDTH + column);
    } else {
        index = (uint8_t)(row * MATRIX_WIDTH + (MATRIX_WIDTH - 1U - column));
    }
#else
    index = (uint8_t)(row * MATRIX_WIDTH + column);
#endif

    matrix_pixels[index] = colour;
}

/** @brief Set every framebuffer pixel to black. */
static void clear_matrix(void)
{
    uint8_t index;

    for (index = 0U; index < WS2812B_LED_COUNT; ++index) {
        matrix_pixels[index] = (ws2812b_rgb_t){.green = 0U, .red = 0U, .blue = 0U};
    }
}

/**
 * @brief Scale all framebuffer channels by a fractional brightness.
 * @param numerator Brightness numerator.
 * @param denominator Brightness denominator; must be non-zero.
 */
static void fade_matrix(uint8_t numerator, uint8_t denominator)
{
    uint8_t index;

    for (index = 0U; index < WS2812B_LED_COUNT; ++index) {
        matrix_pixels[index].red = (uint8_t)((uint16_t)matrix_pixels[index].red * numerator / denominator);
        matrix_pixels[index].green = (uint8_t)((uint16_t)matrix_pixels[index].green * numerator / denominator);
        matrix_pixels[index].blue = (uint8_t)((uint16_t)matrix_pixels[index].blue * numerator / denominator);
    }
}

/** @brief Return a colour scaled by a 0-255 brightness value. */
static ws2812b_rgb_t scale_colour(ws2812b_rgb_t colour, uint8_t brightness)
{
    colour.red = (uint8_t)((uint16_t)colour.red * brightness / 255U);
    colour.green = (uint8_t)((uint16_t)colour.green * brightness / 255U);
    colour.blue = (uint8_t)((uint16_t)colour.blue * brightness / 255U);
    return colour;
}

/** @brief Convert an 8-bit colour-wheel hue into a fully saturated GRB colour. */
static ws2812b_rgb_t hue_to_colour(uint8_t hue)
{
    const uint8_t segment = hue / 43U;
    const uint8_t offset = (uint8_t)((hue - segment * 43U) * 6U);
    const uint8_t rising = offset;
    const uint8_t falling = (uint8_t)(255U - offset);

    switch (segment) {
    case 0U: return (ws2812b_rgb_t){.green = rising,  .red = 255U,    .blue = 0U};
    case 1U: return (ws2812b_rgb_t){.green = 255U,    .red = falling, .blue = 0U};
    case 2U: return (ws2812b_rgb_t){.green = 255U,    .red = 0U,      .blue = rising};
    case 3U: return (ws2812b_rgb_t){.green = falling, .red = 0U,      .blue = 255U};
    case 4U: return (ws2812b_rgb_t){.green = 0U,      .red = rising,  .blue = 255U};
    default: return (ws2812b_rgb_t){.green = 0U,      .red = 255U,    .blue = falling};
    }
}

/** @brief Clear and initialise all state used by a newly selected animation. */
static void initialise_animation(animation_t animation)
{
    uint8_t column;

    clear_matrix();
    switch (animation) {
    case ANIMATION_RAIN:
        for (column = 0U; column < MATRIX_WIDTH; ++column) {
            rain_row[column] = -(int8_t)(random_u8() % 8U);
            rain_colour[column] = random_colour();
        }
        break;
    case ANIMATION_RAINBOW_WAVE:
        rainbow_phase = 0U;
        break;
    case ANIMATION_COMET:
        comet_position = 0U;
        comet_colour = random_colour();
        break;
    case ANIMATION_STARFIELD:
        break;
    case ANIMATION_COLOUR_WIPE:
        wipe_position = 0U;
        wipe_colour = random_colour();
        break;
    case ANIMATION_THEATER_CHASE:
        chase_phase = 0U;
        break;
    case ANIMATION_SCANNER:
        scanner_row = 0;
        scanner_direction = 1;
        rainbow_phase = 0U;
        break;
    case ANIMATION_PLASMA:
        plasma_phase = 0U;
        break;
    case ANIMATION_3D_CUBE:
        break;
    case ANIMATION_3D_TUNNEL:
        tunnel_phase = 0U;
        break;
    }
}

/** @brief Advance multicolour falling drops and their fading trails by one frame. */
static void step_rain(void)
{
    uint8_t column;

    fade_matrix(3U, 5U);
    for (column = 0U; column < MATRIX_WIDTH; ++column) {
        if (rain_row[column] >= 0) {
            matrix_set_pixel((uint8_t)rain_row[column], column,
                             scale_colour(rain_colour[column],
                                          (uint8_t)(150U + (random_u8() & 0x3FU))));
        }

        ++rain_row[column];
        if (rain_row[column] >= (int8_t)MATRIX_HEIGHT) {
            rain_row[column] = -(int8_t)((random_u8() % 7U) + 1U);
            rain_colour[column] = random_colour();
        }
    }
}

/** @brief Render one advancing diagonal rainbow-wave frame. */
static void step_rainbow_wave(void)
{
    uint8_t row;
    uint8_t column;

    for (row = 0U; row < MATRIX_HEIGHT; ++row) {
        for (column = 0U; column < MATRIX_WIDTH; ++column) {
            matrix_set_pixel(row, column,
                             scale_colour(hue_to_colour((uint8_t)(rainbow_phase + row * 19U + column * 25U)), 96U));
        }
    }
    rainbow_phase += 11U;
}

/** @brief Advance a vertical, colour-changing comet with a fading trail. */
static void step_comet(void)
{
    uint8_t row;

    fade_matrix(1U, 2U);
    for (row = 0U; row < MATRIX_HEIGHT; ++row) {
        matrix_set_pixel(row, comet_position, comet_colour);
    }
    ++comet_position;
    if (comet_position >= MATRIX_WIDTH) {
        comet_position = 0U;
        comet_colour = random_colour();
    }
}

/** @brief Render randomly appearing, fading multicolour stars. */
static void step_starfield(void)
{
    uint8_t star_count;

    fade_matrix(2U, 3U);
    for (star_count = 0U; star_count < 2U; ++star_count) {
        matrix_set_pixel(random_u8() % MATRIX_HEIGHT, random_u8() % MATRIX_WIDTH,
                         scale_colour(hue_to_colour(random_u8()), 140U));
    }
}

/** @brief Advance a solid-colour progressive matrix wipe. */
static void step_colour_wipe(void)
{
    uint8_t index;

    clear_matrix();
    for (index = 0U; index <= wipe_position; ++index) {
        matrix_set_pixel(index / MATRIX_WIDTH, index % MATRIX_WIDTH, wipe_colour);
    }
    ++wipe_position;
    if (wipe_position >= WS2812B_LED_COUNT) {
        wipe_position = 0U;
        wipe_colour = random_colour();
    }
}

/** @brief Advance a three-phase, rainbow theatre-chase pattern. */
static void step_theater_chase(void)
{
    uint8_t row;
    uint8_t column;

    clear_matrix();
    for (row = 0U; row < MATRIX_HEIGHT; ++row) {
        for (column = 0U; column < MATRIX_WIDTH; ++column) {
            if (((row + column + chase_phase) % 3U) == 0U) {
                matrix_set_pixel(row, column,
                                 scale_colour(hue_to_colour((uint8_t)(row * 35U + column * 17U + chase_phase * 28U)), 180U));
            }
        }
    }
    ++chase_phase;
    if (chase_phase >= 3U) {
        chase_phase = 0U;
    }
}

/** @brief Advance a bouncing horizontal scanner beam. */
static void step_scanner(void)
{
    uint8_t column;

    fade_matrix(1U, 2U);
    for (column = 0U; column < MATRIX_WIDTH; ++column) {
        matrix_set_pixel((uint8_t)scanner_row, column, hue_to_colour(rainbow_phase));
    }
    rainbow_phase += 17U;

    if (scanner_row >= (int8_t)(MATRIX_HEIGHT - 1U)) {
        scanner_direction = -1;
    } else if (scanner_row <= 0) {
        scanner_direction = 1;
    }
    scanner_row += scanner_direction;
}

/** @brief Render one procedural, colour-shifting plasma frame. */
static void step_plasma(void)
{
    uint8_t row;
    uint8_t column;

    for (row = 0U; row < MATRIX_HEIGHT; ++row) {
        for (column = 0U; column < MATRIX_WIDTH; ++column) {
            const uint8_t pattern = (uint8_t)(plasma_phase + row * row * 15U +
                                              column * column * 19U + row * column * 11U);
            matrix_set_pixel(row, column, scale_colour(hue_to_colour(pattern), 110U));
        }
    }
    plasma_phase += 9U;
}

/**
 * @brief Rasterise one 2D projection edge using Bresenham's line algorithm.
 * @param first_vertex Packed 0xXY source coordinate.
 * @param second_vertex Packed 0xXY destination coordinate.
 * @param colour Colour to assign to every line pixel.
 */
static void draw_cube_edge(uint8_t first_vertex, uint8_t second_vertex, ws2812b_rgb_t colour)
{
    int8_t first_column = (int8_t)(first_vertex >> 4U);
    int8_t first_row = (int8_t)(first_vertex & 0x0FU);
    const int8_t second_column = (int8_t)(second_vertex >> 4U);
    const int8_t second_row = (int8_t)(second_vertex & 0x0FU);
    const int8_t delta_column = (first_column < second_column) ?
        (second_column - first_column) : (first_column - second_column);
    const int8_t column_step = (first_column < second_column) ? 1 : -1;
    const int8_t delta_row = (first_row < second_row) ?
        (first_row - second_row) : (second_row - first_row);
    const int8_t row_step = (first_row < second_row) ? 1 : -1;
    int8_t error = (int8_t)(delta_column + delta_row);

    for (;;) {
        matrix_set_pixel((uint8_t)first_row, (uint8_t)first_column, colour);
        if ((first_column == second_column) && (first_row == second_row)) {
            break;
        }

        {
            const int8_t doubled_error = (int8_t)(2 * error);

            if (doubled_error >= delta_row) {
                error = (int8_t)(error + delta_row);
                first_column += column_step;
            }
            if (doubled_error <= delta_column) {
                error = (int8_t)(error + delta_column);
                first_row += row_step;
            }
        }
    }
}

/** @brief Render the fixed two-face, four-depth-edge wireframe cube. */
static void step_3d_cube(void)
{
    const ws2812b_rgb_t cube_colour = {.green = 180U, .red = 30U, .blue = 255U};
    uint8_t edge;

    clear_matrix();

    for (edge = 0U; edge < (sizeof(cube_edges) / sizeof(cube_edges[0])); ++edge) {
        const uint8_t first_index = pgm_read_byte(&cube_edges[edge][0]);
        const uint8_t second_index = pgm_read_byte(&cube_edges[edge][1]);
        const uint8_t first_vertex = pgm_read_byte(&cube_vertices[first_index]);
        const uint8_t second_vertex = pgm_read_byte(&cube_vertices[second_index]);

        draw_cube_edge(first_vertex, second_vertex, cube_colour);
    }
}

/** @brief Draw one centred square used as a perspective layer in the tunnel. */
static void draw_tunnel_square(uint8_t size, ws2812b_rgb_t colour)
{
    const uint8_t first = (MATRIX_WIDTH - size) / 2U;
    const uint8_t last = (uint8_t)(first + size - 1U);
    uint8_t position;

    for (position = first; position <= last; ++position) {
        matrix_set_pixel(first, position, colour);
        matrix_set_pixel(last, position, colour);
        matrix_set_pixel(position, first, colour);
        matrix_set_pixel(position, last, colour);
    }
}

/** @brief Advance the expanding, colour-shifting nested-square tunnel. */
static void step_3d_tunnel(void)
{
    uint8_t layer;

    clear_matrix();
    for (layer = 0U; layer < 3U; ++layer) {
        const uint8_t size = (uint8_t)(((tunnel_phase + layer * 3U) % MATRIX_WIDTH) + 1U);
        draw_tunnel_square(size, scale_colour(hue_to_colour((uint8_t)(tunnel_phase * 22U + layer * 75U)), 145U));
    }
    ++tunnel_phase;
    if (tunnel_phase >= MATRIX_WIDTH) {
        tunnel_phase = 0U;
    }
}

/** @brief Update the selected effect and transmit the completed LED frame. */
static void step_animation(animation_t animation)
{
    switch (animation) {
    case ANIMATION_RAIN:          step_rain(); break;
    case ANIMATION_RAINBOW_WAVE:  step_rainbow_wave(); break;
    case ANIMATION_COMET:         step_comet(); break;
    case ANIMATION_STARFIELD:     step_starfield(); break;
    case ANIMATION_COLOUR_WIPE:   step_colour_wipe(); break;
    case ANIMATION_THEATER_CHASE: step_theater_chase(); break;
    case ANIMATION_SCANNER:       step_scanner(); break;
    case ANIMATION_PLASMA:        step_plasma(); break;
    case ANIMATION_3D_CUBE:       step_3d_cube(); break;
    case ANIMATION_3D_TUNNEL:     step_3d_tunnel(); break;
    }
    ws2812b_write(matrix_pixels, WS2812B_LED_COUNT);
}

/** @brief Queue the complete serial menu from program flash. */
static void print_menu(void)
{
    uart_write_flash(PSTR("\r\n=== RGB Matrix Animator ===\r\n"));
    uart_write_flash(PSTR("1 - Digital rain\r\n2 - Rainbow wave\r\n3 - Multicolour comet\r\n"));
    uart_write_flash(PSTR("4 - Twinkling starfield\r\n5 - Colour wipe\r\n6 - Theater chase\r\n"));
    uart_write_flash(PSTR("7 - Rainbow scanner\r\n8 - Plasma\r\n9 - Static 3D wireframe cube\r\n"));
    uart_write_flash(PSTR("0 - 3D colour tunnel\r\nm - Show this menu\r\nSend a number (0-9): "));
}

/**
 * @brief Decode a UART menu key into an animation identifier.
 * @return true for a recognised selection key; otherwise false.
 */
static bool select_animation(uint8_t input, animation_t *animation)
{
    switch (input) {
    case '1': *animation = ANIMATION_RAIN; return true;
    case '2': *animation = ANIMATION_RAINBOW_WAVE; return true;
    case '3': *animation = ANIMATION_COMET; return true;
    case '4': *animation = ANIMATION_STARFIELD; return true;
    case '5': *animation = ANIMATION_COLOUR_WIPE; return true;
    case '6': *animation = ANIMATION_THEATER_CHASE; return true;
    case '7': *animation = ANIMATION_SCANNER; return true;
    case '8': *animation = ANIMATION_PLASMA; return true;
    case '9': *animation = ANIMATION_3D_CUBE; return true;
    case '0': *animation = ANIMATION_3D_TUNNEL; return true;
    default: return false;
    }
}

/**
 * @brief Initialise drivers and run the menu/animation state machine forever.
 * @return Never returns on the target MCU.
 */
int main(void)
{
    app_state_t state = APP_SHOW_MENU;
    animation_t selected_animation = ANIMATION_RAIN;

    ws2812b_init();
    uart_init();
    sei();

    for (;;) {
        uint8_t input;

        while (uart_read_byte(&input)) {
            if ((input == 'm') || (input == 'M')) {
                state = APP_SHOW_MENU;
            } else if (select_animation(input, &selected_animation)) {
                initialise_animation(selected_animation);
                state = APP_RUNNING_ANIMATION;
                uart_write_flash(PSTR("\r\nAnimation started. Send m for menu.\r\n"));
            }
        }

        if (state == APP_SHOW_MENU) {
            print_menu();
            state = APP_RUNNING_ANIMATION;
            initialise_animation(selected_animation);
        }

        step_animation(selected_animation);
        _delay_ms(ANIMATION_FRAME_DELAY_MS);
    }
}
