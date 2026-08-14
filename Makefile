# Host-side build for the STM32 EEPROM emulation library's test suite.
# No cross-compiler or target hardware required - eeprom.c is portable
# standard C and runs against test/test_eeprom.c's RAM-backed mock flash.
#
# Usage:
#   make        # build test/test_eeprom
#   make run    # build and run it
#   make clean

CC      ?= gcc
STD     ?= c11
CFLAGS  := -std=$(STD) -Wall -Wextra -Werror -Wpedantic -Iinc -O1 -g

SRC     := src/eeprom.c test/test_eeprom.c
BIN     := test/test_eeprom

.PHONY: all run clean

all: $(BIN)

$(BIN): $(SRC) inc/eeprom.h
	$(CC) $(CFLAGS) -o $(BIN) $(SRC)

run: $(BIN)
	./$(BIN)

clean:
	rm -f $(BIN)
