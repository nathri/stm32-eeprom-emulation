# STM32 EEPROM Emulation Library Specification

## 1. Purpose & Scope

**Purpose**: Provide a production-grade EEPROM emulation library for STM32 microcontrollers that lack built-in EEPROM, using Flash memory sectors with automatic wear-leveling.

**Target Use Cases**:
- Configuration/calibration data storage in production automotive firmware
- Parameter persistence in IoT/industrial devices
- Non-volatile settings storage when EEPROM unavailable

**Approach**: Record-based log with garbage collection (industry-standard, used by ST, Nordic, Zephyr)

---

## 2. Target Hardware

**Supported MCUs**:
- STM32F1 series (F100, F103, etc.) — no built-in EEPROM, 1KB/2KB flash sectors
- STM32F4 series (F405, F407, etc.) — no built-in EEPROM, 16KB/64KB flash sectors
- STM32L4 series (L476, etc.) — no built-in EEPROM, 2KB flash sectors

**Flash Characteristics**:
- Erase cycles: ~10,000 (F1/F4) to ~100,000 (L4)
- Write granularity: 32-bit (F1/F4) or 64-bit (L4)
- Erase at sector level (minimum unit)

---

## 3. Constraints

- **Virtual address space**: 0x0000 to 0x0FFF (4096 possible variables max)
- **Max data per entry**: 255 bytes (fits in uint8_t size field)
- **Write latency**: <10ms per operation (no blocking waits)
- **Response time for read**: <5ms average
- **Thread-safety**: Supports single-threaded + ISR context (no spinlocks)
- **Initialization time**: <100ms for scanning all sectors
- **Recovery on corrupted sector**: Automatic, transparent to caller

---

## 4. Memory Layout

### 4.1 Flash Allocation Example (STM32F4)

```
Flash Base: 0x08000000

Sector 11 (128KB @ 0x080E0000) — EEPROM sectors [reserved application storage]
├── Sector 11A (64KB) — EEPROM working area, sector 0
├── Sector 11B (64KB) — EEPROM working area, sector 1
└── Sector 12 (128KB) — EEPROM working area, sector 2
```

**Config**:
```c
#define EEPROM_FLASH_START   0x080E0000
#define EEPROM_SECTOR_SIZE   (64 * 1024)  // 64KB sectors
#define EEPROM_NUM_SECTORS   3             // 3 sectors = 192KB usable
#define EEPROM_VIRTUAL_SIZE  (4 * 1024)    // 4KB virtual address space
```

### 4.2 On-Flash Structure

Each Flash sector holds a small header (tracking the sector's state and
wear/erase history) followed by a log of variable-length records. Free
space is tracked without any additional redundant bookkeeping, consistent
with real NOR-Flash constraints (a sector can only be erased as a whole;
individual bits can only be cleared, not set, between erases).

### 4.3 Records

Each record carries the virtual address it belongs to, its data size
(1-255 bytes), the data itself, and a CRC16 checksum. Records are
append-only — writing to an address that already has a value adds a new
record rather than modifying data in place. A record left partially
written by a power loss is reliably detected and discarded during
recovery, never mistaken for valid data.

### 4.4 Virtual Address Space

- **Addresses**: 0x0000 to 0x0FFF (4096 possible variables)
- **Max size per address**: 255 bytes
- **Multiple writes to same address**: New record added, old invalidated
- **Latest write wins**: reading an address always returns the most recently written value for it.

---

## 5. API Specification

### 5.1 Error Codes

```c
typedef enum {
    EEPROM_OK                 = 0,   // Operation successful
    EEPROM_UNINITIALIZED      = 1,   // Library not initialized
    EEPROM_INVALID_ADDR       = 2,   // Address out of range
    EEPROM_INVALID_SIZE       = 3,   // Data size > 255 bytes
    EEPROM_WRITE_FAILED       = 4,   // Flash write error
    EEPROM_ERASE_FAILED       = 5,   // Flash erase error
    EEPROM_SECTOR_FULL        = 6,   // All sectors full (need GC)
    EEPROM_NOT_FOUND          = 7,   // Variable never written
    EEPROM_CHECKSUM_ERROR     = 8,   // Data corrupted
    EEPROM_FORMAT_ERROR       = 9,   // Flash layout corrupted
    EEPROM_READ_FAILED        = 10,  // Flash read callback reported a physical I/O error
} eeprom_status_t;
```

### 5.2 Configuration Structure

```c
typedef struct {
    uint32_t flash_start;           // Base address of EEPROM sectors
    uint32_t sector_size;           // Size of each sector (bytes)
    uint8_t num_sectors;            // Number of sectors to use (2–8 recommended)
    uint8_t write_width;            // Flash write granularity (4 for 32-bit, 8 for 64-bit)
    
    // Flash driver callbacks (platform-specific)
    int (*flash_read)(uint32_t addr, uint8_t *data, uint16_t size);
    int (*flash_write)(uint32_t addr, const uint8_t *data, uint16_t size);
    int (*flash_erase)(uint32_t addr, uint32_t size);
} eeprom_config_t;
```

### 5.3 Statistics Structure

```c
typedef struct {
    uint32_t total_writes;          // Lifetime write operations
    uint32_t total_reads;           // Lifetime read operations
    uint32_t gc_runs;               // Garbage collection runs
    uint32_t erase_count[8];        // Per-sector erase counts
    uint32_t sector_usage[8];       // Per-sector bytes used
    uint8_t sector_state[8];        // Per-sector state (see eeprom_get_stats())
    uint8_t oldest_sector;          // Next sector for GC
} eeprom_stats_t;
```

### 5.4 Core API

```c
/**
 * Initialize EEPROM library.
 * Scans all sectors, builds lookup table, performs recovery if needed.
 *
 * @param config Configuration (flash_start, num_sectors, callbacks, etc.)
 * @return EEPROM_OK on success, error code on failure
 *
 * Note: Must be called once before any read/write operations.
 */
eeprom_status_t eeprom_init(const eeprom_config_t *config);

/**
 * Write data to virtual EEPROM address.
 * Appends record to active sector. Triggers GC if sector full.
 *
 * @param addr Virtual address (0–0x0FFF)
 * @param data Pointer to data
 * @param size Number of bytes (1–255)
 * @return EEPROM_OK on success, error code on failure
 *
 * Note: Non-blocking. Updates written immediately (durability guaranteed).
 */
eeprom_status_t eeprom_write(uint16_t addr, const uint8_t *data, uint8_t size);

/**
 * Read data from virtual EEPROM address.
 * Searches for latest valid record at addr and returns data.
 *
 * @param addr Virtual address (0–0x0FFF)
 * @param data Pointer to output buffer (must be >= original size)
 * @param size [in] Buffer size, [out] Bytes read
 * @return EEPROM_OK on success, EEPROM_NOT_FOUND if addr never written
 *
 * Note: CRC verified automatically. Returns EEPROM_CHECKSUM_ERROR if corrupted.
 */
eeprom_status_t eeprom_read(uint16_t addr, uint8_t *data, uint8_t *size);

/**
 * Check if variable exists at address.
 * Returns true if address has been written at least once.
 *
 * @param addr Virtual address
 * @return true if exists, false otherwise
 */
bool eeprom_exists(uint16_t addr);

/**
 * Get library statistics (writes, reads, GC runs, erase counts).
 *
 * @param stats Pointer to statistics struct
 * @return EEPROM_OK on success
 */
eeprom_status_t eeprom_get_stats(eeprom_stats_t *stats);

/**
 * Force garbage collection (optional, called automatically).
 * Compacts oldest full sector and erases it.
 *
 * @return EEPROM_OK on success
 *
 * Note: Takes ~10–50ms depending on sector size. Do not call in tight loops.
 */
eeprom_status_t eeprom_garbage_collect(void);

/**
 * Reset all EEPROM data (erase all sectors).
 * Dangerous: Called only for factory reset / initialization.
 *
 * @return EEPROM_OK on success
 */
eeprom_status_t eeprom_format(void);
```

### 5.5 Flash Driver Abstraction (Porting Layer)

Platform code must provide:

```c
// Read raw flash
int flash_read_impl(uint32_t addr, uint8_t *data, uint16_t size) {
    memcpy(data, (void *)addr, size);
    return 0;  // 0 = success
}

// Write to flash (address must be erased, aligned to write_width)
int flash_write_impl(uint32_t addr, const uint8_t *data, uint16_t size) {
    // Call HAL_FLASH_Program() or equivalent
    // Return 0 on success, -1 on error
}

// Erase flash sector
int flash_erase_impl(uint32_t addr, uint32_t size) {
    // Call HAL_FLASHEx_Erase() or equivalent
    // Return 0 on success, -1 on error
}

// Register with library
eeprom_config_t cfg = {
    .flash_start = 0x080E0000,
    .sector_size = 64 * 1024,
    .num_sectors = 3,
    .write_width = 4,
    .flash_read = flash_read_impl,
    .flash_write = flash_write_impl,
    .flash_erase = flash_erase_impl,
};
eeprom_init(&cfg);
```

---

## 6. Wear-Leveling Algorithm

### 6.1 Initialization

On initialization, the library scans all managed sectors and rebuilds an
in-RAM index of the latest value for every variable before accepting any
read or write. If a record left behind by an interrupted write (e.g. a
power loss) is found during that scan, it's discarded automatically;
initialization completes normally and the library remains fully
operational.

### 6.2 Writes

A write validates its address and size, then appends a new, checksummed
record to the currently active sector and updates the in-RAM index to
point at it. If the active sector doesn't have room for the new record,
garbage collection runs automatically to reclaim space before the write
completes.

### 6.3 Garbage Collection

Garbage collection reclaims the oldest full sector: every still-current
record in it is preserved by copying it to a destination sector, after
which the reclaimed sector is erased and returned to the pool of
available space. Only records that are still the current value for their
address survive the copy; superseded and invalid records are dropped.

### 6.4 Wear-Leveling Strategy

**Round-robin sector rotation**: every sector cycles through the same
states at close to the same rate, so erase counts across all sectors
stay within about 1 of each other under sustained load. See
`IMPLEMENTATION_NOTES.md`'s "Key design decisions" for how this is
achieved.

---

## 7. Error Handling & Recovery

### 7.1 Corruption Detection

- **CRC16 per record** — Validates data integrity
- **Sector header validation** — Detects incomplete writes
- **Sequence number checks** — Detects out-of-order sectors

### 7.2 Recovery Strategy

Recovery from a corrupted sector header, a corrupted record, or a record
left partially written by a power loss is automatic and transparent to
the caller — see `IMPLEMENTATION_NOTES.md`'s "Known limitations" for the
specific edge cases still being hardened.

---

## 8. Thread Safety

**Single-threaded with ISR safety**:
- No locks/mutexes (suitable for bare-metal)
- ISRs can call eeprom_read() safely (read-only)
- **Critical**: Only one task can call eeprom_write() at a time
- If RTOS in use, protect eeprom_write() with mutex

**Atomic write guarantee**:
- Each record write is atomic (flash hardware guarantees)
- Partial writes detected via CRC and sequence validation

---

## 9. MISRA-C:2012 Compliance

The implementation targets MISRA-C:2012 compliance, verified with static
analysis. See `IMPLEMENTATION_NOTES.md`'s "MISRA-C:2012 Deviation
Record" for the authoritative, tool-verified compliance statement and
the specific, reviewed deviations.

---

## 10. Testing & Validation

See EEPROM_TEST_CASES.md for comprehensive test scenarios.

**Validation checklist**:
- [x] All test cases pass (TC-001 through TC-021, plus 4 audit/regression tests — 25 total)
- [x] MISRA-C compliance verified with static analyzer
- [x] No compiler warnings at -Wall -Wextra
- [ ] Flash driver callbacks tested on actual hardware
- [x] Wear-leveling verified (erase counts distributed equally)
- [x] Power-loss simulation tested (recovery works)

---

## 11. API Usage Example

```c
#include "eeprom.h"

// Configure EEPROM
eeprom_config_t cfg = {
    .flash_start = 0x080E0000,
    .sector_size = 64 * 1024,
    .num_sectors = 3,
    .write_width = 4,
    .flash_read = flash_read,
    .flash_write = flash_write,
    .flash_erase = flash_erase,
};

int main(void) {
    // Initialize
    if (eeprom_init(&cfg) != EEPROM_OK) {
        Error_Handler();
    }
    
    // Write config data
    uint8_t config[4] = {0x11, 0x22, 0x33, 0x44};
    eeprom_write(0x0001, config, 4);
    
    // Read it back
    uint8_t buffer[4];
    uint8_t size = 4;
    if (eeprom_read(0x0001, buffer, &size) == EEPROM_OK) {
        printf("Read: %02X %02X %02X %02X\n", 
               buffer[0], buffer[1], buffer[2], buffer[3]);
    }
    
    // Check statistics
    eeprom_stats_t stats;
    eeprom_get_stats(&stats);
    printf("GC runs: %lu, Total writes: %lu\n", 
           stats.gc_runs, stats.total_writes);
    
    return 0;
}
```

---

**End of Specification**
