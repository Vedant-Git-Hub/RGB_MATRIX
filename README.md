# RGB Matrix Animator

Bare-metal AVR firmware for an Arduino Nano (ATmega328P, 16 MHz) driving a
16×16 WS2812B RGB LED matrix made from four 8×8 panels. It uses no Arduino
core or Arduino libraries.

The project provides a cycle-accurate one-wire WS2812B output on **D13**, an
interrupt-buffered UART menu at 115200 baud, and ten selectable LED effects.

## Features

- Works with a 256-pixel (16×16) WS2812B matrix assembled from four 8×8 panels.
- Uses D13/PB5 for matrix `DIN`; it does not require the hardware SPI MOSI pin.
- Provides a UART menu on the Nano USB serial connection, including an
  autonomous snake game.
- Keeps menu text, palettes, and animation geometry in program flash.
- Uses about 952 B SRAM and 4 KB flash in the current build.
- Includes Doxygen comments and a `Doxyfile` for generated API documentation.

## Hardware connections

### Signal and power wiring

```text
                    Arduino Nano                         WS2812B 16×16 matrix
              ┌─────────────────────┐                  ┌────────────────────┐
              │                 D13 │──[330–470 Ω]────>│ DIN                │
              │                     │                  │                    │
              │                 GND │─────────────────>│ GND                │
              └─────────────────────┘                  │                    │
                                                       │ +5V                │
      External regulated 5 V supply + ───────────────>│ 5V                 │
      External regulated 5 V supply − ──┬─────────────>│ GND                │
                                         └─────────────> Nano GND
                                                       └────────────────────┘
```

| Nano | ATmega328P | Matrix | Notes |
| --- | --- | --- | --- |
| D13 | PB5 / SCK | `DIN` | Matrix data signal through a 330–470 Ω resistor |
| GND | GND | `GND` | Must be common with the external supply |
| — | — | `5V` | Use a separate regulated 5 V matrix supply |

### Panel chain order

```text
  +----------------+----------------+
  |  Panel 0       |  Panel 1       |
  |  top-left      |  top-right     |
  |  DIN → DOUT    |  DIN → DOUT    |
  +----------------+----------------+
  |  Panel 2       |  Panel 3       |
  |  bottom-left   |  bottom-right  |
  |  DIN → DOUT    |  DIN → DOUT    |
  +----------------+----------------+

  Data chain: Nano D13 → Panel 0 → Panel 1 → Panel 2 → Panel 3
```

All panels should face the same way. On each panel, LED 0 is at VIN/bottom-right;
the chain travels bottom-right to bottom-left, then the next row left-to-right,
continuing upward in a serpentine pattern. The firmware applies the measured
horizontal mirror correction for this wiring and maps the four panels into one
16×16 logical coordinate system using the physical chain order `top-left,
top-right, bottom-left, bottom-right`.

Connect to the matrix **`DIN`**, not `DOUT`. D13 is normally the Nano's SPI
clock pin; because its physical pin is required here, firmware bit-bangs the
WS2812B waveform with cycle-timed AVR instructions. Hardware SPI data remains
on D11 and is not used.

### Power and signal safety

- Do **not** power all 256 LEDs from the Nano USB or 5 V pin. A full-brightness
  white matrix can draw several amps; size the external 5 V supply accordingly.
- Connect the external supply ground to Nano GND before applying power.
- Place a 1000 µF or larger electrolytic capacitor across matrix `5V` and
  `GND`, near the matrix.
- A standard 5 V Nano can normally drive a 5 V WS2812B `DIN` directly. A 3.3 V
  controller needs a suitable 5 V logic-level shifter, such as a 74AHCT125.
- D13 also drives the Nano's onboard LED. If your matrix is far away or signal
  quality is poor, use a 74AHCT125 buffer/level shifter close to the matrix.

## Build prerequisites

On Debian/Ubuntu systems:

```sh
sudo apt install gcc-avr avr-libc avrdude doxygen
```

`doxygen` is required only for generating the API reference.

## Build, flash, and use

Build the firmware:

```sh
make
```

Flash it. The Makefile auto-detects the newest `/dev/ttyACM*` or
`/dev/ttyUSB*` device and uses the Nano Optiboot baud rate of 115200:

```sh
make flash
```

To select the port explicitly:

```sh
make flash PORT=/dev/ttyUSB0
```

Some older Nano bootloaders require 57600 baud:

```sh
make flash PORT=/dev/ttyUSB0 BAUD=57600
```

Open the same serial device at **115200 baud, 8-N-1, no flow control**. The
firmware prints its menu on startup. Send a single key to select an effect;
send `m` at any time to print the menu again.

| Key | Animation |
| --- | --- |
| `1` | Digital rain |
| `2` | Rainbow wave |
| `3` | Multicolour comet |
| `4` | Twinkling starfield |
| `5` | Colour wipe |
| `6` | Theater chase |
| `7` | Rainbow scanner |
| `8` | Plasma |
| `9` | Rotating colour 3D wireframe cube |
| `0` | 3D colour tunnel |
| `s` | Autonomous snake game |
| `t` | 16×16 row-wise panel orientation test |

The snake plays without user input. Food is allocated randomly on unoccupied
cells, the snake grows after each meal, and after eating 10 pieces it flashes
a red/orange border-and-cross game-over animation before automatically starting
a new round.

The `t` test treats the assembly as one continuous 16×16 canvas. It clears the
matrix and lights one white logical pixel every 100 ms in row-major order:
(row 0, column 0) through (row 0, column 15), then row 1, through row 15.
The driver translates those logical coordinates into each panel's serpentine
physical chain order. Use this to verify the complete 16×16 layout before
testing animations.

The application checks UART commands once each 150 ms animation frame. UART
receive/transmit run through USART interrupts. The WS2812B data output masks
interrupts for approximately 7.5 ms per frame, because an interrupt during
the waveform could be interpreted as the WS2812B reset interval.

## Matrix orientation

The logical display is mapped for the bottom-right-origin serpentine layout
inside each 8×8 panel, including the horizontal mirror correction verified with
the `t` UART test. That test lights the complete logical 16×16 canvas row by row
at 100 ms intervals, starting at logical `(0,0)` in the top-left corner.

If a different panel wiring is used, adjust `matrix_physical_index()` in
`src/main.c` and rebuild. Do not change the logical animation code; it always
uses `(row, column)` coordinates on the assembled 16×16 canvas.

## Project layout

```text
include/       Public UART, WS2812B, and D13 serial-driver interfaces
src/           Drivers, state machine, menu, and animation implementations
Makefile       avr-gcc build, avrdude flash, and Doxygen documentation targets
Doxyfile       Doxygen configuration
```

## API documentation

Generate HTML documentation into `docs/html/index.html`:

```sh
make docs
```

The generated `docs/` directory is intentionally ignored by Git.

## Design notes

`ws2812b_rgb_t` stores bytes in **GRB** order—the order required by a WS2812B.
This lets the driver stream the application framebuffer directly, avoiding a
second 768-byte output buffer. The colour palette, strings, cube geometry, and
rotation table are declared with `PROGMEM`, keeping them in flash rather than
consuming SRAM. Rain animation colours are stored as compact palette indices,
and the UART transmit queue is flow-controlled at 64 bytes to reduce SRAM use.

## License

This project is licensed under the GNU General Public License v3.0. See
[LICENSE](LICENSE) for the full terms.
