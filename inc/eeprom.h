/**
 * @file    eeprom.h
 * @brief   STM32 EEPROM emulation library - public API.
 *
 * Record-based log-structured storage over Flash sectors, with automatic
 * wear-leveling and garbage collection. Suitable for parameter/calibration
 * persistence on STM32 parts that lack a built-in EEPROM peripheral.
 *
 * Usage:
 *   1. Implement flash_read/flash_write/flash_erase for your platform.
 *   2. Populate an eeprom_config_t and call eeprom_init().
 *   3. Use eeprom_write()/eeprom_read() to persist variables by virtual
 *      address (0x0000-0x0FFF).
 *
 * Thread safety: eeprom_read() and eeprom_exists() are safe to call from
 * an ISR (read-only, no shared mutable state is written). eeprom_write(),
 * eeprom_garbage_collect() and eeprom_format() mutate library state and
 * MUST NOT be called concurrently with each other or with eeprom_init();
 * if used from an RTOS, serialize them with a mutex.
 */

#ifndef EEPROM_H
#define EEPROM_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum number of Flash sectors the library can manage. */
#define EEPROM_MAX_SECTORS ((uint8_t)8U)

/**
 * Highest legal virtual address (inclusive). Defaults to the spec's
 * 0x0000-0x0FFF (4096 addresses), which costs a 16 KiB RAM lookup table
 * (g_lookup, one uint32_t per address) - too large for small-RAM parts
 * such as the STM32F103C8 (20 KiB total). Override by defining this macro
 * (e.g. via a build-system -D flag, or a #define before #include
 * "eeprom.h" in a single translation unit that also includes eeprom.c) to
 * the actual number of variables your application needs minus one, e.g.
 * ((uint16_t)0x003FU) for 64 addresses costs only 256 bytes of RAM.
 *
 * Must stay below 0xFF00: eeprom.c's free-space detection relies on a
 * fully-erased record header (0xFF 0xFF 0xFF 0xFF) never being a
 * legitimate record, which requires the address high byte to never reach
 * 0xFF. eeprom.c enforces this bound at compile time.
 *
 * Deliberately not cast to (uint16_t): the value must remain usable in a
 * preprocessor #if (eeprom.c's bound check above), and C casts are not
 * valid in constant preprocessor expressions. The plain unsigned literal
 * still converts implicitly and correctly everywhere the library compares
 * it against a uint16_t address.
 */
#ifndef EEPROM_MAX_VIRTUAL_ADDR
#define EEPROM_MAX_VIRTUAL_ADDR (0x0FFFU)
#endif

/**
 * @brief Status/error codes returned by the public API.
 */
typedef enum {
    EEPROM_OK                 = 0, /**< Operation successful. */
    EEPROM_UNINITIALIZED       = 1, /**< Library not initialized. */
    EEPROM_INVALID_ADDR        = 2, /**< Address out of range. */
    EEPROM_INVALID_SIZE        = 3, /**< Data size out of range (must be 1-255). */
    EEPROM_WRITE_FAILED        = 4, /**< Flash write callback reported an error. */
    EEPROM_ERASE_FAILED        = 5, /**< Flash erase callback reported an error. */
    EEPROM_SECTOR_FULL         = 6, /**< All sectors full; garbage collection could not free space. */
    EEPROM_NOT_FOUND           = 7, /**< Variable has never been written. */
    EEPROM_CHECKSUM_ERROR      = 8, /**< Stored record failed CRC verification. */
    EEPROM_FORMAT_ERROR        = 9, /**< Flash layout is corrupted beyond recovery. */
    EEPROM_READ_FAILED         = 10 /**< Flash read callback reported a physical I/O error (data may be intact; the read itself failed). */
} eeprom_status_t;

/**
 * @brief Platform configuration and Flash driver callbacks.
 *
 * All three callbacks must return 0 on success and a non-zero value on
 * failure. flash_write() is only ever called on addresses that are either
 * freshly erased or being narrowed (bits cleared, never set) by the caller.
 */
typedef struct {
    uint32_t flash_start;   /**< Base address of the first EEPROM-emulation sector. */
    uint32_t sector_size;   /**< Size of each sector in bytes (identical for all sectors). */
    uint8_t  num_sectors;   /**< Number of sectors to manage (2-EEPROM_MAX_SECTORS). */
    uint8_t  write_width;   /**< Flash program granularity in bytes (4 or 8). */

    /** Read @p size bytes from Flash at @p addr into @p data. Returns 0 on success. */
    int (*flash_read)(uint32_t addr, uint8_t *data, uint16_t size);

    /** Program @p size bytes of @p data to Flash at @p addr. Returns 0 on success. */
    int (*flash_write)(uint32_t addr, const uint8_t *data, uint16_t size);

    /** Erase @p size bytes starting at @p addr (sector granularity). Returns 0 on success. */
    int (*flash_erase)(uint32_t addr, uint32_t size);
} eeprom_config_t;

/**
 * @brief Runtime usage statistics, primarily for diagnostics/telemetry.
 */
typedef struct {
    uint32_t total_writes;                       /**< Lifetime count of eeprom_write() calls that succeeded. */
    uint32_t total_reads;                        /**< Lifetime count of eeprom_read() calls that succeeded. */
    uint32_t gc_runs;                             /**< Lifetime count of completed garbage-collection cycles. */
    uint32_t erase_count[EEPROM_MAX_SECTORS];     /**< Per-sector lifetime erase count. */
    uint32_t sector_usage[EEPROM_MAX_SECTORS];    /**< Per-sector bytes currently used (from sector header to write pointer). */
    uint8_t  oldest_sector;                       /**< Index of the sector that will be targeted by the next garbage collection. */
} eeprom_stats_t;

/**
 * @brief Initialize the EEPROM emulation library.
 *
 * Scans all configured sectors, rebuilds the in-RAM lookup table from the
 * newest valid records, and performs recovery of any partially-written
 * record left behind by a power loss. Must be called exactly once before
 * any other API function (a re-init after a reset is expected and safe).
 *
 * @param config  Configuration and Flash driver callbacks. The pointer is
 *                not retained beyond copying the struct contents; the
 *                caller may free/reuse it after this call returns. The
 *                function pointers inside it must remain valid for the
 *                library's lifetime.
 * @return EEPROM_OK on success; EEPROM_FORMAT_ERROR if the Flash layout is
 *         unrecoverable; EEPROM_ERASE_FAILED/EEPROM_WRITE_FAILED if a
 *         required recovery operation failed.
 */
eeprom_status_t eeprom_init(const eeprom_config_t *config);

/**
 * @brief Write (or overwrite) a variable at a virtual address.
 *
 * Appends a new record to the active sector. The previous record at the
 * same address, if any, is invalidated. Triggers an internal garbage
 * collection automatically if the active sector is full.
 *
 * @param addr  Virtual address, 0x0000-0x0FFF.
 * @param data  Pointer to the source data. Must not be NULL.
 * @param size  Number of bytes to store, 1-255.
 * @return EEPROM_OK on success; EEPROM_UNINITIALIZED, EEPROM_INVALID_ADDR,
 *         EEPROM_INVALID_SIZE, EEPROM_WRITE_FAILED or EEPROM_SECTOR_FULL
 *         on failure.
 */
eeprom_status_t eeprom_write(uint16_t addr, const uint8_t *data, uint8_t size);

/**
 * @brief Read the latest value stored at a virtual address.
 *
 * @param addr  Virtual address, 0x0000-0x0FFF.
 * @param data  Output buffer. Must be at least *size bytes. Must not be NULL.
 * @param size  In: capacity of @p data in bytes. Out: on EEPROM_OK, the
 *              number of bytes actually written to @p data; on
 *              EEPROM_INVALID_SIZE (buffer too small), the number of bytes
 *              the stored record actually needs, so the caller can size a
 *              retry buffer. Must not be NULL.
 * @return EEPROM_OK on success; EEPROM_UNINITIALIZED, EEPROM_INVALID_ADDR,
 *         EEPROM_NOT_FOUND (never written), EEPROM_CHECKSUM_ERROR (stored
 *         record's CRC does not match its data - the record itself is
 *         corrupted), or EEPROM_READ_FAILED (the flash_read() callback
 *         reported a physical I/O error - the stored data may well be
 *         intact, only the read attempt failed) on failure. @p data is
 *         left unmodified on any failure.
 */
eeprom_status_t eeprom_read(uint16_t addr, uint8_t *data, uint8_t *size);

/**
 * @brief Check whether a virtual address currently holds a valid value.
 *
 * @param addr  Virtual address, 0x0000-0x0FFF.
 * @return true if a valid record is stored at @p addr, false otherwise
 *         (including when the library is uninitialized or addr is out of
 *         range).
 */
bool eeprom_exists(uint16_t addr);

/**
 * @brief Retrieve a snapshot of runtime statistics.
 *
 * @param stats  Output struct, must not be NULL.
 * @return EEPROM_OK on success; EEPROM_UNINITIALIZED if not yet initialized.
 */
eeprom_status_t eeprom_get_stats(eeprom_stats_t *stats);

/**
 * @brief Force a garbage-collection cycle.
 *
 * Normally triggered automatically by eeprom_write(); exposed for callers
 * that want to pay the latency cost at a controlled point in time (e.g.
 * an idle task) rather than during a write. Takes on the order of
 * milliseconds to tens of milliseconds depending on sector size - do not
 * call from a tight loop or a hard real-time path.
 *
 * @return EEPROM_OK on success (including the no-op case where no sector
 *         needs collecting); EEPROM_UNINITIALIZED, EEPROM_ERASE_FAILED or
 *         EEPROM_WRITE_FAILED on failure.
 */
eeprom_status_t eeprom_garbage_collect(void);

/**
 * @brief Erase all managed sectors and discard all stored data.
 *
 * Intended for factory reset only. This is irreversible.
 *
 * @return EEPROM_OK on success; EEPROM_UNINITIALIZED or EEPROM_ERASE_FAILED
 *         on failure.
 */
eeprom_status_t eeprom_format(void);

#ifdef __cplusplus
}
#endif

#endif /* EEPROM_H */
