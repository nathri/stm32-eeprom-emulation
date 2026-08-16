# Changelog

This project has no release/version scheme yet (no tags, no `VERSION`
file) — entries below are grouped as "Unreleased" until that changes.
Only API-relevant changes are tracked here; see `IMPLEMENTATION_NOTES.md`
for the full engineering writeup (bugs found, root causes, verification)
behind each of these.

## Unreleased

### Added

- `eeprom_stats_t` gained a new field, `sector_state[EEPROM_MAX_SECTORS]`
  (added directly before the existing `oldest_sector` field), plus four
  new public constants: `EEPROM_SECTOR_STATE_EMPTY`,
  `EEPROM_SECTOR_STATE_ACTIVE`, `EEPROM_SECTOR_STATE_FULL`,
  `EEPROM_SECTOR_STATE_GC_DEST`. `eeprom_get_stats()` now reports each
  managed sector's current state (previously private to `eeprom.c` — see
  `test/test_eeprom.c`'s `TC-018`, which used to document that as a
  black-box limitation). Diagnostics only: no on-flash format change, no
  new failure modes, nothing else about the public API's behavior
  changed. Regression-tested by `TC-021`.

  **Compatibility note — read this if you have existing code against
  this library:**
  - **Not affected:** code that does `eeprom_stats_t stats = {0};` (or
    any other whole-struct/aggregate initialization), or that only reads
    the fields it already knew about (`total_writes`, `erase_count`,
    `sector_usage`, etc.) by name.
  - **Affected — audit before upgrading:** any code that hardcodes
    `sizeof(eeprom_stats_t)` (e.g. for a fixed-size buffer, a memory pool
    slab, or a struct-layout assertion), or that serializes/transmits
    `eeprom_stats_t` as a raw byte blob (e.g. writing it straight to a
    log, a debug UART frame, or another Flash region) and expects a
    fixed byte layout on the far end. The struct is now 4 bytes larger
    per configured `EEPROM_MAX_SECTORS` (one `uint8_t` per sector slot);
    anything depending on the old size or field offsets of
    `oldest_sector` will silently read/write the wrong bytes if not
    updated to match.
