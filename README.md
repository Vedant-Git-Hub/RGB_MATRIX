# RGB Matrix Animator

Bare-metal AVR firmware for an Arduino Nano (ATmega328P, 16 MHz) driving an
8×8 WS2812B RGB LED matrix. It uses no Arduino core or Arduino libraries.

The project provides a cycle-accurate one-wire WS2812B output on **D13**, an
interrupt-buffered UART menu at 115200 baud, and ten selectable LED effects.

## Features

- Works with a 64-pixel (8×8) WS2812B matrix.
- Uses D13/PB5 for matrix `DIN`; it does not require the hardware SPI MOSI pin.
- Provides a UART menu on the Nano USB serial connection.
- Keeps menu text, palettes, and animation geometry in program flash.
- Uses only 392 B SRAM and about 3 KB flash in the current build.
- Includes Doxygen comments and a `Doxyfile` for generated API documentation.

## Hardware connections

### Signal and power wiring

```text
                    Arduino Nano                         WS2812B 8×8 matrix
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

Connect to the matrix **`DIN`**, not `DOUT`. D13 is normally the Nano's SPI
clock pin; because its physical pin is required here, firmware bit-bangs the
WS2812B waveform with cycle-timed AVR instructions. Hardware SPI data remains
on D11 and is not used.

### Power and signal safety

- Do **not** power all 64 LEDs from the Nano USB or 5 V pin. A full-brightness
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
| `9` | Static 3D wireframe cube |
| `0` | 3D colour tunnel |

The application checks UART commands once each 100 ms animation frame. UART
receive/transmit run through USART interrupts. The WS2812B data output masks
interrupts for approximately 1.92 ms per frame, because an interrupt during
the waveform could be interpreted as the WS2812B reset interval.

## Matrix orientation

The logical display is mapped for the common serpentine layout: alternating
matrix rows are wired in opposite directions. If your panel's rows all run in
the same direction, change this setting in `src/main.c` and rebuild:

```c
#define MATRIX_ROWS_ARE_SERPENTINE 0U
```

If the animation is upside down or mirrored, rotate the matrix physically or
adjust `matrix_set_pixel()` in `src/main.c` to match its actual `DIN` corner.

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
second 192-byte output buffer. The colour palette, strings, and cube geometry
are declared with `PROGMEM`, keeping them in flash rather than consuming SRAM.

## License

This project is licensed under the GNU General Public License v3.0. See
[LICENSE](LICENSE) for the full terms.
