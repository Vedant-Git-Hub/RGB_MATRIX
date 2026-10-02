# Standalone ATmega328P / Arduino Nano build
MCU       := atmega328p
F_CPU     := 16000000UL
TARGET    := rgb_matrix_animator
BUILD_DIR := build

CC      := avr-gcc
OBJCOPY := avr-objcopy
SIZE    := avr-size
AVRDUDE := avrdude

# The newest USB serial device is normally the Nano just connected.
# Override when needed, e.g. make flash PORT=/dev/ttyACM0
PORT       ?= $(shell ls -t /dev/ttyACM* /dev/ttyUSB* 2>/dev/null | head -n 1)
PROGRAMMER ?= arduino
BAUD       ?= 115200

CPPFLAGS := -DF_CPU=$(F_CPU) -Iinclude
CFLAGS   := -mmcu=$(MCU) -std=gnu11 -Os -Wall -Wextra -Wpedantic \
            -ffunction-sections -fdata-sections -flto
LDFLAGS  := -mmcu=$(MCU) -Wl,--gc-sections -flto

SOURCES := $(wildcard src/*.c)
OBJECTS := $(SOURCES:src/%.c=$(BUILD_DIR)/%.o)
ELF     := $(BUILD_DIR)/$(TARGET).elf
HEX     := $(BUILD_DIR)/$(TARGET).hex

.PHONY: all flash size docs clean

all: $(HEX)

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/%.o: src/%.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(ELF): $(OBJECTS)
	$(CC) $(LDFLAGS) $^ -o $@

$(HEX): $(ELF)
	$(OBJCOPY) -O ihex -R .eeprom $< $@
	$(SIZE) --mcu=$(MCU) --format=avr $<

flash: $(HEX)
	@if [ -z "$(PORT)" ]; then \
		echo "No /dev/ttyACM* or /dev/ttyUSB* device found; connect the Nano or set PORT explicitly."; \
		exit 2; \
	fi
	@echo "Flashing $(PORT) at $(BAUD) baud..."
	$(AVRDUDE) -p $(MCU) -c $(PROGRAMMER) -P $(PORT) -b $(BAUD) -D -U flash:w:$(HEX):i

size: $(ELF)
	$(SIZE) --mcu=$(MCU) --format=avr $<

docs:
	doxygen Doxyfile

clean:
	rm -rf $(BUILD_DIR)
