/**
 * @file main.c
 * @brief Animation state machine and UART-controlled 16x16 matrix application.
 *
 * The foreground loop drains UART events, applies the selected animation to a
 * logical framebuffer, and refreshes the WS2812B chain every 150 ms.
 */

#include <stdbool.h>
#include <stdint.h>

#include <avr/interrupt.h>
#include <avr/pgmspace.h>
#include <util/delay.h>

#include "uart.h"
#include "ws2812b.h"

#define MATRIX_WIDTH  16U  /**< Logical matrix width in pixels. */
#define MATRIX_HEIGHT 16U  /**< Logical matrix height in pixels. */
#define MATRIX_MODULE_SIZE 8U  /**< Width/height of each physical module. */
#define ANIMATION_FRAME_DELAY_MS 150U  /**< Normal animation frame interval. */
#define PANEL_TEST_DELAY_MS 100U  /**< Orientation-test frame interval. */
#define SNAKE_MAX_LENGTH 32U  /**< Storage capacity for the autonomous snake. */
#define SNAKE_TARGET_FOOD 10U  /**< Foods required before game over. */

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
    ANIMATION_3D_TUNNEL,
    ANIMATION_SNAKE,
    ANIMATION_PANEL_TEST
} animation_t;

/** @brief Saturated animation colours stored in program flash. */
static const ws2812b_rgb_t colour_palette[] PROGMEM = {
    {.green = 24U,  .red = 255U, .blue = 0U},
    {.green = 180U, .red = 255U, .blue = 0U},
    {.green = 255U, .red = 0U,   .blue = 32U},
    {.green = 180U, .red = 0U,   .blue = 255U},
    {.green = 64U,  .red = 32U,  .blue = 255U},
    {.green = 0U,   .red = 220U, .blue = 255U}
};

/** @brief Number of entries in the flash-resident animation palette. */
#define COLOUR_PALETTE_COUNT (sizeof(colour_palette) / sizeof(colour_palette[0]))

/** @brief Sine values for 16 equally spaced rotation angles, scaled by 64. */
static const int8_t cube_sine[16] PROGMEM = {
    0, 25, 45, 59, 64, 59, 45, 25,
    0, -25, -45, -59, -64, -59, -45, -25
};

/**
 * @brief Cube vertices as signed X/Y/Z coordinates in program flash.
 * Vertices 0-3 form the rear face and 4-7 form the front face.
 */
static const int8_t cube_vertices[8][3] PROGMEM = {
    {-3, -3, -3}, { 3, -3, -3}, { 3,  3, -3}, {-3,  3, -3},
    {-3, -3,  3}, { 3, -3,  3}, { 3,  3,  3}, {-3,  3,  3}
};

/** @brief Four edges per face, then the four edges that convey depth. */
static const uint8_t cube_edges[][2] PROGMEM = {
    {0U, 1U}, {1U, 2U}, {2U, 3U}, {3U, 0U},
    {4U, 5U}, {5U, 6U}, {6U, 7U}, {7U, 4U},
    {0U, 4U}, {1U, 5U}, {2U, 6U}, {3U, 7U}
};

/** @brief Logical 16x16 framebuffer stored in native WS2812B GRB order. */
static ws2812b_rgb_t matrix_pixels[WS2812B_LED_COUNT];
/** @brief Deterministic pseudo-random generator state. */
static uint16_t random_state = 0xC0DEU;
/** @brief Falling-rain row and colour state for each logical column. */
static int8_t rain_row[MATRIX_WIDTH];
/** @brief Flash-palette index for each falling-rain column. */
static uint8_t rain_colour_index[MATRIX_WIDTH];
/** @brief Phase of the diagonal rainbow-wave animation. */
static uint8_t rainbow_phase;
/** @brief Current column of the fading comet. */
static uint8_t comet_position;
/** @brief Current comet colour. */
static ws2812b_rgb_t comet_colour;
/** @brief Number of pixels revealed by the colour-wipe animation. */
static uint16_t wipe_position;
/** @brief Current colour-wipe colour. */
static ws2812b_rgb_t wipe_colour;
/** @brief Phase of the theater-chase colour pattern. */
static uint8_t chase_phase;
/** @brief Current row of the bouncing scanner. */
static int8_t scanner_row;
/** @brief Vertical direction of the scanner, either -1 or +1. */
static int8_t scanner_direction;
/** @brief Phase of the procedural plasma animation. */
static uint8_t plasma_phase;
/** @brief Phase of the nested-square 3D tunnel. */
static uint8_t tunnel_phase;
/** @brief Current 16-step angle used by the rotating cube. */
static uint8_t cube_angle;
/** @brief Snake body cells, with the head at index zero. */
static uint8_t snake_body[SNAKE_MAX_LENGTH];
/** @brief Number of occupied cells in the snake body. */
static uint8_t snake_length;
/** @brief Current food cell in logical row-major coordinates. */
static uint8_t snake_food;
/** @brief Number of food cells eaten in the current round. */
static uint8_t snake_food_eaten;
/** @brief Non-zero while the snake game-over animation is active. */
static uint8_t snake_game_over;
/** @brief Current frame of the snake game-over animation. */
static uint8_t snake_game_over_frame;
/** @brief Next logical cell used by the continuous panel test. */
static uint16_t panel_test_position;

/** @brief Forward declaration because animation initialisation precedes snake code. */
static void initialise_snake(void);

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

/** @brief Read one saturated colour from the flash-resident palette. */
static ws2812b_rgb_t palette_colour(uint8_t index)
{
    ws2812b_rgb_t colour;

    colour.green = pgm_read_byte(&colour_palette[index].green);
    colour.red = pgm_read_byte(&colour_palette[index].red);
    colour.blue = pgm_read_byte(&colour_palette[index].blue);
    return colour;
}

/** @brief Choose one saturated colour from the flash-resident palette. */
static ws2812b_rgb_t random_colour(void)
{
    return palette_colour((uint8_t)(random_u8() % COLOUR_PALETTE_COUNT));
}

/**
 * @brief Convert a logical 16x16 coordinate into the chained LED index.
 *
 * Every panel starts at its VIN/bottom-right corner: the bottom row is 0..7
 * from right to left, the row above is 8..15 from left to right, and the
 * sequence continues upward. Panel offsets follow the cable order top-left,
 * top-right, bottom-left, bottom-right.
 */
static uint16_t matrix_physical_index(uint8_t row, uint8_t column)
{
    const uint8_t module_row = row / MATRIX_MODULE_SIZE;
    const uint8_t module_column = column / MATRIX_MODULE_SIZE;
    const uint8_t local_row = row & (MATRIX_MODULE_SIZE - 1U);
    const uint8_t chain_row = (uint8_t)(MATRIX_MODULE_SIZE - 1U - local_row);
    const uint8_t local_column = column & (MATRIX_MODULE_SIZE - 1U);
    uint8_t module;
    uint8_t physical_column = local_column;
    uint16_t index;

    /* Physical chain order is TL, TR, BL, BR. */
    if (module_row == 0U) {
        module = module_column;
    } else {
        module = (uint8_t)(2U + module_column);
    }

    /* Mirror each local row so every logical row scans left-to-right visually. */
    physical_column = (uint8_t)(MATRIX_MODULE_SIZE - 1U - physical_column);

    index = (uint16_t)module * (MATRIX_MODULE_SIZE * MATRIX_MODULE_SIZE) +
            (uint16_t)chain_row * MATRIX_MODULE_SIZE + physical_column;
    return index;
}

/**
 * @brief Set one logical pixel while applying the panel's wiring map.
 * @param row Logical row, 0 through 15.
 * @param column Logical column, 0 through 15.
 * @param colour GRB colour to write.
 */
static void matrix_set_pixel(uint8_t row, uint8_t column, ws2812b_rgb_t colour)
{
    matrix_pixels[matrix_physical_index(row, column)] = colour;
}

/** @brief Set every framebuffer pixel to black. */
static void clear_matrix(void)
{
    uint16_t index;

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
    uint16_t index;

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
            rain_row[column] = -(int8_t)(random_u8() % MATRIX_HEIGHT);
            rain_colour_index[column] = (uint8_t)(random_u8() % COLOUR_PALETTE_COUNT);
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
        cube_angle = 0U;
        break;
    case ANIMATION_3D_TUNNEL:
        tunnel_phase = 0U;
        break;
    case ANIMATION_SNAKE:
        initialise_snake();
        break;
    case ANIMATION_PANEL_TEST:
        panel_test_position = 0U;
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
                             scale_colour(palette_colour(rain_colour_index[column]),
                                          (uint8_t)(150U + (random_u8() & 0x3FU))));
        }

        ++rain_row[column];
        if (rain_row[column] >= (int8_t)MATRIX_HEIGHT) {
            rain_row[column] = -(int8_t)((random_u8() % 7U) + 1U);
            rain_colour_index[column] = (uint8_t)(random_u8() % COLOUR_PALETTE_COUNT);
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
    uint16_t index;

    clear_matrix();
    for (index = 0U; index <= wipe_position; ++index) {
        matrix_set_pixel((uint8_t)(index / MATRIX_WIDTH),
                         (uint8_t)(index % MATRIX_WIDTH), wipe_colour);
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

/** @brief Clamp a projected coordinate to the 16x16 display boundary. */
static uint8_t clamp_cube_coordinate(int16_t coordinate)
{
    if (coordinate < 0) {
        return 0U;
    }
    if (coordinate >= (int16_t)MATRIX_WIDTH) {
        return (uint8_t)(MATRIX_WIDTH - 1U);
    }
    return (uint8_t)coordinate;
}

/** @brief Render one rotated, hue-shifting wireframe cube frame. */
static void step_3d_cube(void)
{
    const uint8_t angle = (uint8_t)(cube_angle & 0x0FU);
    const uint8_t x_angle = (uint8_t)((angle + 4U) & 0x0FU);
    const int16_t sin_y = (int16_t)(int8_t)pgm_read_byte(&cube_sine[angle]);
    const int16_t cos_y = (int16_t)(int8_t)pgm_read_byte(&cube_sine[(angle + 4U) & 0x0FU]);
    const int16_t sin_x = (int16_t)(int8_t)pgm_read_byte(&cube_sine[x_angle]);
    const int16_t cos_x = (int16_t)(int8_t)pgm_read_byte(&cube_sine[(x_angle + 4U) & 0x0FU]);
    const ws2812b_rgb_t cube_colour = scale_colour(hue_to_colour((uint8_t)(cube_angle * 16U)), 210U);
    uint8_t projected_vertices[8];
    uint8_t edge;
    uint8_t vertex;

    clear_matrix();

    for (vertex = 0U; vertex < 8U; ++vertex) {
        const int16_t x = (int16_t)(int8_t)pgm_read_byte(&cube_vertices[vertex][0]);
        const int16_t y = (int16_t)(int8_t)pgm_read_byte(&cube_vertices[vertex][1]);
        const int16_t z = (int16_t)(int8_t)pgm_read_byte(&cube_vertices[vertex][2]);
        const int16_t rotated_x = (int16_t)((x * cos_y + z * sin_y) / 64);
        const int16_t rotated_z = (int16_t)((-x * sin_y + z * cos_y) / 64);
        const int16_t rotated_y = (int16_t)((y * cos_x - rotated_z * sin_x) / 64);
        const int16_t projected_column = 8 + rotated_x * 2 + rotated_z / 2;
        const int16_t projected_row = 8 + rotated_y * 2 - rotated_z / 2;

        projected_vertices[vertex] = (uint8_t)(clamp_cube_coordinate(projected_column) << 4U);
        projected_vertices[vertex] |= clamp_cube_coordinate(projected_row);
    }

    for (edge = 0U; edge < (sizeof(cube_edges) / sizeof(cube_edges[0])); ++edge) {
        const uint8_t first_index = pgm_read_byte(&cube_edges[edge][0]);
        const uint8_t second_index = pgm_read_byte(&cube_edges[edge][1]);

        draw_cube_edge(projected_vertices[first_index], projected_vertices[second_index], cube_colour);
    }

    cube_angle = (uint8_t)((cube_angle + 1U) & 0x0FU);
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

/** @brief Return whether a cell is currently occupied by the snake body. */
static bool snake_contains(uint8_t cell)
{
    uint8_t index;

    for (index = 0U; index < snake_length; ++index) {
        if (snake_body[index] == cell) {
            return true;
        }
    }
    return false;
}

/** @brief Select a random food cell that is not occupied by the snake. */
static void snake_place_food(void)
{
    do {
        snake_food = random_u8();
    } while (snake_contains(snake_food));
}

/** @brief Initialise a new autonomous snake round at the centre of the grid. */
static void initialise_snake(void)
{
    clear_matrix();
    snake_length = 3U;
    snake_body[0] = (uint8_t)(8U * MATRIX_WIDTH + 8U);
    snake_body[1] = (uint8_t)(8U * MATRIX_WIDTH + 7U);
    snake_body[2] = (uint8_t)(8U * MATRIX_WIDTH + 6U);
    snake_food_eaten = 0U;
    snake_game_over = 0U;
    snake_game_over_frame = 0U;
    snake_place_food();
}

/**
 * @brief Choose a safe next cell, preferring a route toward the food.
 * @param next_cell Receives the selected cell when one is available.
 * @return true when a legal move exists, false when the snake is trapped.
 */
static bool snake_choose_next(uint8_t *next_cell)
{
    const uint8_t head = snake_body[0];
    const int8_t head_row = (int8_t)(head / MATRIX_WIDTH);
    const int8_t head_column = (int8_t)(head % MATRIX_WIDTH);
    const int8_t food_row = (int8_t)(snake_food / MATRIX_WIDTH);
    const int8_t food_column = (int8_t)(snake_food % MATRIX_WIDTH);
    const int8_t row_distance = food_row - head_row;
    const int8_t column_distance = food_column - head_column;
    const uint8_t row_distance_abs = (uint8_t)(row_distance < 0 ? -row_distance : row_distance);
    const uint8_t column_distance_abs = (uint8_t)(column_distance < 0 ? -column_distance : column_distance);
    uint8_t directions[4];
    uint8_t direction_count = 0U;
    uint8_t direction;

    /* Try the axis with the larger distance first, then the other axis. */
    if (column_distance_abs >= row_distance_abs) {
        if (column_distance < 0) {
            directions[direction_count++] = 3U;
        } else if (column_distance > 0) {
            directions[direction_count++] = 1U;
        }
        if (row_distance < 0) {
            directions[direction_count++] = 0U;
        } else if (row_distance > 0) {
            directions[direction_count++] = 2U;
        }
    } else {
        if (row_distance < 0) {
            directions[direction_count++] = 0U;
        } else if (row_distance > 0) {
            directions[direction_count++] = 2U;
        }
        if (column_distance < 0) {
            directions[direction_count++] = 3U;
        } else if (column_distance > 0) {
            directions[direction_count++] = 1U;
        }
    }

    for (direction = 0U; direction < 4U; ++direction) {
        uint8_t candidate_direction = direction;
        bool already_added = false;
        uint8_t index;

        for (index = 0U; index < direction_count; ++index) {
            if (directions[index] == candidate_direction) {
                already_added = true;
            }
        }
        if (!already_added) {
            directions[direction_count++] = candidate_direction;
        }
    }

    for (direction = 0U; direction < direction_count; ++direction) {
        int8_t next_row = head_row;
        int8_t next_column = head_column;

        switch (directions[direction]) {
        case 0U: --next_row; break;
        case 1U: ++next_column; break;
        case 2U: ++next_row; break;
        default: --next_column; break;
        }

        if ((next_row >= 0) && (next_row < (int8_t)MATRIX_HEIGHT) &&
            (next_column >= 0) && (next_column < (int8_t)MATRIX_WIDTH)) {
            const uint8_t candidate = (uint8_t)(next_row * MATRIX_WIDTH + next_column);

            if (!snake_contains(candidate)) {
                *next_cell = candidate;
                return true;
            }
        }
    }
    return false;
}

/** @brief Draw the flashing red/orange game-over pattern. */
static void step_snake_game_over(void)
{
    const ws2812b_rgb_t colour = ((snake_game_over_frame & 1U) == 0U) ?
        (ws2812b_rgb_t){.green = 0U, .red = 255U, .blue = 0U} :
        (ws2812b_rgb_t){.green = 50U, .red = 255U, .blue = 0U};
    uint8_t row;
    uint8_t column;

    clear_matrix();
    for (row = 0U; row < MATRIX_HEIGHT; ++row) {
        for (column = 0U; column < MATRIX_WIDTH; ++column) {
            if ((row == 0U) || (column == 0U) ||
                (row == MATRIX_HEIGHT - 1U) || (column == MATRIX_WIDTH - 1U) ||
                (row == column) || (column == MATRIX_WIDTH - 1U - row)) {
                matrix_set_pixel(row, column, colour);
            }
        }
    }
    ++snake_game_over_frame;
    if (snake_game_over_frame >= 10U) {
        initialise_snake();
    }
}

/** @brief Advance the autonomous snake, food, growth, and ten-food limit. */
static void step_snake(void)
{
    const ws2812b_rgb_t food_colour = {.green = 100U, .red = 255U, .blue = 0U};
    uint8_t index;
    uint8_t next_cell;
    bool ate_food;

    if (snake_game_over != 0U) {
        step_snake_game_over();
        return;
    }

    clear_matrix();
    matrix_set_pixel(snake_food / MATRIX_WIDTH, snake_food % MATRIX_WIDTH, food_colour);
    for (index = 0U; index < snake_length; ++index) {
        const uint8_t brightness = (index == 0U) ? 255U : (uint8_t)(220U - index * 10U);
        const ws2812b_rgb_t body_colour = {
            .green = brightness, .red = (uint8_t)(brightness / 8U), .blue = (uint8_t)(brightness / 4U)
        };

        matrix_set_pixel(snake_body[index] / MATRIX_WIDTH,
                         snake_body[index] % MATRIX_WIDTH, body_colour);
    }

    if (!snake_choose_next(&next_cell)) {
        snake_game_over = 1U;
        snake_game_over_frame = 0U;
        return;
    }

    ate_food = (next_cell == snake_food);
    if (ate_food && (snake_length < SNAKE_MAX_LENGTH)) {
        ++snake_length;
    }
    for (index = snake_length - 1U; index > 0U; --index) {
        snake_body[index] = snake_body[index - 1U];
    }
    snake_body[0] = next_cell;

    if (ate_food) {
        ++snake_food_eaten;
        if (snake_food_eaten >= SNAKE_TARGET_FOOD) {
            snake_game_over = 1U;
            snake_game_over_frame = 0U;
        } else {
            snake_place_food();
        }
    }
}

/** @brief Light one logical pixel at a time for 16x16 wiring verification. */
static void step_panel_test(void)
{
    const ws2812b_rgb_t test_colour = {.green = 255U, .red = 255U, .blue = 255U};

    clear_matrix();
    /* Walk one continuous logical 16x16 canvas in row-major order. */
    matrix_set_pixel((uint8_t)(panel_test_position / MATRIX_WIDTH),
                     (uint8_t)(panel_test_position % MATRIX_WIDTH), test_colour);
    ++panel_test_position;
    if (panel_test_position >= WS2812B_LED_COUNT) {
        panel_test_position = 0U;
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
    case ANIMATION_SNAKE:         step_snake(); break;
    case ANIMATION_PANEL_TEST:    step_panel_test(); break;
    }
    ws2812b_write(matrix_pixels, (uint16_t)WS2812B_LED_COUNT);
}

/** @brief Queue the complete serial menu from program flash. */
static void print_menu(void)
{
    uart_write_flash(PSTR("\r\n=== RGB Matrix Animator ===\r\n"));
    uart_write_flash(PSTR("1 - Digital rain\r\n2 - Rainbow wave\r\n3 - Multicolour comet\r\n"));
    uart_write_flash(PSTR("4 - Twinkling starfield\r\n5 - Colour wipe\r\n6 - Theater chase\r\n"));
    uart_write_flash(PSTR("7 - Rainbow scanner\r\n8 - Plasma\r\n9 - Rotating colour 3D cube\r\n"));
    uart_write_flash(PSTR("0 - 3D colour tunnel\r\ns - Autonomous snake\r\n"));
    uart_write_flash(PSTR("t - 16x16 row-wise panel test\r\nm - Show this menu\r\nSend a number (0-9/s/t): "));
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
    case 's':
    case 'S': *animation = ANIMATION_SNAKE; return true;
    case 't':
    case 'T': *animation = ANIMATION_PANEL_TEST; return true;
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
        if (selected_animation == ANIMATION_PANEL_TEST) {
            _delay_ms(PANEL_TEST_DELAY_MS);
        } else {
            _delay_ms(ANIMATION_FRAME_DELAY_MS);
        }
    }
}
