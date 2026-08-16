# STM32 EEPROM Emulation Library

A record-based, log-structured EEPROM emulation library for STM32 parts
that lack a built-in EEPROM peripheral (F1/F4/L4 and similar), storing
parameter/calibration data across Flash sectors with automatic
wear-leveling and garbage collection.

<!--
  CI badge: fill in <OWNER>/<REPO> once this is pushed to GitHub.
  https://github.com/<OWNER>/<REPO>/actions/workflows/ci.yml/badge.svg
-->
<!-- ![CI](https://github.com/<OWNER>/<REPO>/actions/workflows/ci.yml/badge.svg) -->

## Features

- Simple `read(addr, data, size)` / `write(addr, data, size)` API over a
  0x0000-0x0FFF virtual address space (configurable, see below).
- Wear-leveling by construction: sectors rotate `EMPTY -> ACTIVE -> FULL ->
  (reclaimed) -> EMPTY` in round-robin order, keeping erase counts across
  sectors within ±1 of each other under sustained load.
- Garbage collection, automatic (triggered by `eeprom_write()` when
  needed) or manual (`eeprom_garbage_collect()`, for paying the latency
  cost at a controlled point such as an idle task).
- CRC16-protected records; automatic recovery from a corrupted sector
  header or a record left partially written by a power loss.
- Portable C11, no dependencies beyond three Flash driver callbacks you
  provide (`flash_read`/`flash_write`/`flash_erase`) and `<string.h>`.
- No dynamic allocation, no recursion, no locks (single-writer by design —
  see "Thread safety" below).

## Status

This is a **beta** library that has been through two rounds of independent
review (see "Independent audit findings" in
[IMPLEMENTATION_NOTES.md](IMPLEMENTATION_NOTES.md)):

- An initial audit that found and led to fixing a critical silent-data-loss
  bug.
- A second review, done during design work ahead of STM32U5 hardware
  bring-up (before any board time, not in response to a failure), which
  found that the original sector-header and record-invalidation design
  relied on reprogramming an already-programmed Flash word — safe on
  plain AND-only Flash, but not reliably supported on STM32U5's
  ECC-protected Flash. Fixed: sector state is now tracked via
  write-once-per-erase-cycle header fields instead of a single mutated
  byte, record invalidation was removed entirely (the in-RAM lookup table
  was already the real source of truth), and a torn/partially-written
  trailing record is now left alone and reclaimed by the next garbage
  collection instead of being written to a second time. `write_width` is
  now fixed at 16 bytes (STM32U5's confirmed quad-word program
  granularity).

It has **not** been run on real STM32 hardware — verification so far is
code review plus a host-simulated test suite (`test/test_eeprom.c`, a
RAM-backed mock Flash) and static analysis; see
[IMPLEMENTATION_NOTES.md](IMPLEMENTATION_NOTES.md) for the full,
currently-accurate list of what has and hasn't been verified, including
where the host test suite itself still needs porting to the new
`write_width=16` layout.

## Usage

```c
#include "eeprom.h"

static int flash_read_impl(uint32_t addr, uint8_t *data, uint16_t size) {
    memcpy(data, (void *)addr, size);
    return 0;
}

static int flash_write_impl(uint32_t addr, const uint8_t *data, uint16_t size) {
    /* HAL_FLASH_Program() or equivalent, one word at a time. */
    return 0; /* 0 = success */
}

static int flash_erase_impl(uint32_t addr, uint32_t size) {
    /* HAL_FLASHEx_Erase() or equivalent. */
    return 0;
}

int main(void) {
    eeprom_config_t cfg = {
        .flash_start = 0x080E0000U,
        .sector_size = 64U * 1024U,
        .num_sectors = 3U,
        .write_width = 16U,  /* fixed: STM32U5 quad-word Flash program granularity */
        .flash_read  = flash_read_impl,
        .flash_write = flash_write_impl,
        .flash_erase = flash_erase_impl,
    };

    if (eeprom_init(&cfg) != EEPROM_OK) {
        Error_Handler();
    }

    uint8_t config_data[4] = { 0x11, 0x22, 0x33, 0x44 };
    eeprom_write(0x0001U, config_data, sizeof(config_data));

    uint8_t buf[4];
    uint8_t size = sizeof(buf);
    if (eeprom_read(0x0001U, buf, &size) == EEPROM_OK) {
        /* buf[0..size-1] holds the stored value */
    }
}
```

Full API reference is in [inc/eeprom.h](inc/eeprom.h) (every function is
documented there — this README doesn't duplicate it).

### Thread safety

`eeprom_read()` and `eeprom_exists()` are safe to call from an ISR
(read-only). `eeprom_write()`, `eeprom_garbage_collect()` and
`eeprom_format()` mutate library state and must not be called concurrently
with each other or with `eeprom_init()` — serialize them with a mutex if
used from an RTOS with more than one writer.

### Reducing RAM usage

The default 0x0000-0x0FFF address space costs 16 KiB of RAM, which is too
large for small-RAM parts like the STM32F103C8 (20 KiB total). If your
application only needs a handful of variables, define
`EEPROM_MAX_VIRTUAL_ADDR` to a smaller value before including `eeprom.h`
(e.g. `0x003FU` for 64 addresses costs only 256 bytes instead of 16 KiB).
See the macro's doc comment in [inc/eeprom.h](inc/eeprom.h) for the one
constraint it must respect.

## Building & testing

No target hardware or cross-compiler needed to run the test suite —
`src/eeprom.c` is portable C11 and runs against a mock Flash on the host:

```sh
make run      # build test/test_eeprom and run all test cases
make clean
```

Requires a C11 compiler (`gcc`/`clang`) on your `PATH`; override with
`make CC=clang`.

## MISRA-C:2012 compliance

**MISRA-C:2012 compliant with documented deviations** (see
[IMPLEMENTATION_NOTES.md](IMPLEMENTATION_NOTES.md), section "MISRA-C:2012
Deviation Record"). Verified with `cppcheck --addon=misra`: zero Required
or Mandatory rule violations; four Advisory-rule deviations, each named,
located, and justified in that document rather than blanket-suppressed.
The same scan runs in CI on every push (`.github/workflows/ci.yml`),
gated by [cppcheck-suppressions.txt](cppcheck-suppressions.txt) — any
finding outside that documented list fails the build.

No MISRA rule text is reproduced here or in this repo (MISRA rule text is
copyrighted by MIRA Ltd / the MISRA Consortium); IMPLEMENTATION_NOTES.md
references rules by number and summarizes their intent only.

## Project layout

```
inc/eeprom.h                 Public API (the only header consumers need)
src/eeprom.c                 Implementation
test/test_eeprom.c           Host test suite (mock Flash, 21 test cases)
Makefile                     Host build (make run)
cppcheck-suppressions.txt    Documented MISRA deviation suppressions
IMPLEMENTATION_NOTES.md      Design rationale, deviation record, audit history
.github/workflows/ci.yml     CI: build+test, and MISRA static analysis
EEPROM_SPEC.md                Original design specification
EEPROM_TEST_CASES.md          Original 20 test-case specification
```

## License

No license file is included yet — add one (e.g. `LICENSE`) before treating
this as usable by anyone outside this repository. Until a license is
present, standard copyright default applies (all rights reserved), which
in practice means no one else can legally use, modify, or redistribute
this code even though it's visible on GitHub.
