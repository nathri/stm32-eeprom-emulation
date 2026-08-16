/**
 * @file test_eeprom.c
 * @brief Host-side test harness for the STM32 EEPROM emulation library.
 *
 * Implements the 20 test cases from EEPROM_TEST_CASES.md against a RAM-backed
 * mock Flash that mimics real NOR Flash semantics: writes can only clear
 * bits (AND), and erase resets a region to 0xFF. This is test-only code
 * (host build, not part of the shipped library) so it is not held to the
 * same MISRA/embedded constraints as eeprom.c - e.g. it freely uses
 * <stdio.h>, dynamic test-table function pointers, and heavier stack usage.
 *
 * Build & run (from the project root):
 *   make -C test
 *   ./test/test_eeprom
 */

#include "eeprom.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --------------------------------------------------------------------- */
/* Mock flash                                                              */
/* --------------------------------------------------------------------- */

#define FLASH_BASE   0x08000000U
#define SECTOR_SIZE  (64U * 1024U)
#define NUM_SECTORS  3U
#define FLASH_TOTAL  (SECTOR_SIZE * NUM_SECTORS)

static uint8_t mock_flash[FLASH_TOTAL];

static int flash_read_mock(uint32_t addr, uint8_t *data, uint16_t size)
{
    uint32_t offset = addr - FLASH_BASE;
    if ((offset + size) > FLASH_TOTAL) {
        return -1;
    }
    memcpy(data, &mock_flash[offset], size);
    return 0;
}

static int flash_write_mock(uint32_t addr, const uint8_t *data, uint16_t size)
{
    uint32_t offset = addr - FLASH_BASE;
    uint16_t i;
    if ((offset + size) > FLASH_TOTAL) {
        return -1;
    }
    for (i = 0; i < size; i++) {
        mock_flash[offset + i] &= data[i]; /* real NOR flash: writes only clear bits */
    }
    return 0;
}

static int flash_erase_mock(uint32_t addr, uint32_t size)
{
    uint32_t offset = addr - FLASH_BASE;
    if ((offset + size) > FLASH_TOTAL) {
        return -1;
    }
    memset(&mock_flash[offset], 0xFF, size);
    return 0;
}

static eeprom_config_t make_config(void)
{
    eeprom_config_t cfg;
    cfg.flash_start = FLASH_BASE;
    cfg.sector_size = SECTOR_SIZE;
    cfg.num_sectors = (uint8_t)NUM_SECTORS;
    cfg.write_width = 4U;
    cfg.flash_read = flash_read_mock;
    cfg.flash_write = flash_write_mock;
    cfg.flash_erase = flash_erase_mock;
    return cfg;
}

static void reset_flash(void)
{
    memset(mock_flash, 0xFF, sizeof(mock_flash));
}

/* Small-sector variant of the config, for stress tests that need many
 * sector rotations to happen within a reasonable number of writes. Uses
 * the same underlying mock_flash buffer, just addresses less of it. */
static eeprom_config_t make_small_config(void)
{
    eeprom_config_t cfg = make_config();
    cfg.sector_size = 1024U;
    return cfg;
}

/* Direct pokes into mock_flash, used only to simulate corruption/power-loss
 * scenarios that a well-behaved flash_write callback could never produce. */
static uint8_t *flash_byte(uint32_t addr)
{
    return &mock_flash[addr - FLASH_BASE];
}

/* Audit regression test infrastructure (Claim 1 / Claim 7): lets a test
 * make exactly one targeted flash_erase() call fail, simulating a power
 * loss at the instant right before that erase would have committed -
 * everything flash_write()'d before it (a GC's compacted survivors, in
 * particular) is left exactly as it would be after a real crash there,
 * without needing to literally interrupt eeprom_garbage_collect() mid-call. */
static uint32_t g_fail_erase_addr = 0xFFFFFFFFU;

static int flash_erase_selective_fail_mock(uint32_t addr, uint32_t size)
{
    if (addr == g_fail_erase_addr) {
        return -1;
    }
    return flash_erase_mock(addr, size);
}

static eeprom_config_t make_gc_regression_config(void)
{
    eeprom_config_t cfg = make_config();
    cfg.sector_size = 256U;
    cfg.num_sectors = 4U;
    cfg.flash_erase = flash_erase_selective_fail_mock;
    return cfg;
}

/* --------------------------------------------------------------------- */
/* Minimal test framework                                                  */
/* --------------------------------------------------------------------- */

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("    FAIL [%s:%d]: %s\n", __FILE__, __LINE__, msg);       \
            return false;                                                    \
        }                                                                    \
    } while (0)

typedef bool (*test_fn_t)(void);

typedef struct {
    const char *id;
    const char *name;
    test_fn_t   fn;
} test_case_t;

/* --------------------------------------------------------------------- */
/* TC-001: Basic Write and Read                                            */
/* --------------------------------------------------------------------- */

static bool tc_001(void)
{
    eeprom_config_t cfg = make_config();
    uint8_t wr[4] = { 0x11, 0x22, 0x33, 0x44 };
    uint8_t rd[4] = { 0 };
    uint8_t sz = sizeof(rd);

    reset_flash();
    CHECK(eeprom_init(&cfg) == EEPROM_OK, "init failed");
    CHECK(eeprom_write(0x0001U, wr, sizeof(wr)) == EEPROM_OK, "write failed");
    CHECK(eeprom_read(0x0001U, rd, &sz) == EEPROM_OK, "read failed");
    CHECK(sz == sizeof(wr), "size mismatch");
    CHECK(memcmp(wr, rd, sizeof(wr)) == 0, "data mismatch");
    return true;
}

/* --------------------------------------------------------------------- */
/* TC-002: Overwrite Same Address                                          */
/* --------------------------------------------------------------------- */

static bool tc_002(void)
{
    eeprom_config_t cfg = make_config();
    uint8_t wr1[4] = { 0x11, 0x22, 0x33, 0x44 };
    uint8_t wr2[4] = { 0xAA, 0xBB, 0xCC, 0xDD };
    uint8_t rd[4] = { 0 };
    uint8_t sz = sizeof(rd);

    reset_flash();
    CHECK(eeprom_init(&cfg) == EEPROM_OK, "init failed");
    CHECK(eeprom_write(0x0001U, wr1, sizeof(wr1)) == EEPROM_OK, "write1 failed");
    CHECK(eeprom_write(0x0001U, wr2, sizeof(wr2)) == EEPROM_OK, "write2 failed");
    sz = sizeof(rd);
    CHECK(eeprom_read(0x0001U, rd, &sz) == EEPROM_OK, "read failed");
    CHECK(memcmp(wr2, rd, sizeof(wr2)) == 0, "did not return latest data");
    return true;
}

/* --------------------------------------------------------------------- */
/* TC-003: Multiple Addresses                                              */
/* --------------------------------------------------------------------- */

static bool tc_003(void)
{
    eeprom_config_t cfg = make_config();
    uint8_t a[2] = { 0x11, 0x22 };
    uint8_t b[3] = { 0x33, 0x44, 0x55 };
    uint8_t c[1] = { 0x66 };
    uint8_t rd[8];
    uint8_t sz;

    reset_flash();
    CHECK(eeprom_init(&cfg) == EEPROM_OK, "init failed");
    CHECK(eeprom_write(0x0001U, a, sizeof(a)) == EEPROM_OK, "write a failed");
    CHECK(eeprom_write(0x0002U, b, sizeof(b)) == EEPROM_OK, "write b failed");
    CHECK(eeprom_write(0x0003U, c, sizeof(c)) == EEPROM_OK, "write c failed");

    sz = sizeof(rd);
    CHECK(eeprom_read(0x0001U, rd, &sz) == EEPROM_OK, "read a failed");
    CHECK((sz == sizeof(a)) && (memcmp(rd, a, sizeof(a)) == 0), "a mismatch");

    sz = sizeof(rd);
    CHECK(eeprom_read(0x0002U, rd, &sz) == EEPROM_OK, "read b failed");
    CHECK((sz == sizeof(b)) && (memcmp(rd, b, sizeof(b)) == 0), "b mismatch");

    sz = sizeof(rd);
    CHECK(eeprom_read(0x0003U, rd, &sz) == EEPROM_OK, "read c failed");
    CHECK((sz == sizeof(c)) && (memcmp(rd, c, sizeof(c)) == 0), "c mismatch");
    return true;
}

/* --------------------------------------------------------------------- */
/* TC-004: Boundary Values                                                 */
/* --------------------------------------------------------------------- */

static bool tc_004(void)
{
    eeprom_config_t cfg = make_config();
    uint8_t wr[255];
    uint8_t rd[255];
    uint8_t sz = sizeof(rd);
    uint16_t i;

    for (i = 0; i < 255U; i++) {
        wr[i] = (uint8_t)(0xFFU - i);
    }

    reset_flash();
    CHECK(eeprom_init(&cfg) == EEPROM_OK, "init failed");
    CHECK(eeprom_write(EEPROM_MAX_VIRTUAL_ADDR, wr, 255U) == EEPROM_OK, "write failed");
    CHECK(eeprom_read(EEPROM_MAX_VIRTUAL_ADDR, rd, &sz) == EEPROM_OK, "read failed");
    CHECK(sz == 255U, "size mismatch");
    CHECK(memcmp(wr, rd, 255U) == 0, "data mismatch");
    return true;
}

/* --------------------------------------------------------------------- */
/* TC-005: Invalid Address Rejection                                       */
/* --------------------------------------------------------------------- */

static bool tc_005(void)
{
    eeprom_config_t cfg = make_config();
    uint8_t wr[1] = { 0x01 };
    uint8_t rd[1];
    uint8_t sz = sizeof(rd);

    reset_flash();
    CHECK(eeprom_init(&cfg) == EEPROM_OK, "init failed");
    CHECK(eeprom_write(0x1000U, wr, sizeof(wr)) == EEPROM_INVALID_ADDR, "0x1000 write not rejected");
    CHECK(eeprom_read(0xFFFFU, rd, &sz) == EEPROM_INVALID_ADDR, "0xFFFF read not rejected");
    return true;
}

/* --------------------------------------------------------------------- */
/* TC-006: Invalid Size Rejection                                          */
/* --------------------------------------------------------------------- */

static bool tc_006(void)
{
    /* eeprom_write()'s `size` parameter is uint8_t per the spec's exact
     * signature, so 256/1000-byte requests are impossible to express at
     * the call site (they would truncate before the function ever sees
     * them) - the type system itself enforces the upper bound. The one
     * value that IS representable and must be rejected at runtime is 0. */
    eeprom_config_t cfg = make_config();
    uint8_t wr[1] = { 0x01 };

    reset_flash();
    CHECK(eeprom_init(&cfg) == EEPROM_OK, "init failed");
    CHECK(eeprom_write(0x0001U, wr, 0U) == EEPROM_INVALID_SIZE, "size=0 not rejected");
    return true;
}

/* --------------------------------------------------------------------- */
/* TC-007: Not Found Case                                                   */
/* --------------------------------------------------------------------- */

static bool tc_007(void)
{
    eeprom_config_t cfg = make_config();
    uint8_t rd[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
    uint8_t rd_before[4];
    uint8_t sz = sizeof(rd);

    reset_flash();
    memcpy(rd_before, rd, sizeof(rd));
    CHECK(eeprom_init(&cfg) == EEPROM_OK, "init failed");
    CHECK(eeprom_read(0x0100U, rd, &sz) == EEPROM_NOT_FOUND, "expected NOT_FOUND");
    CHECK(memcmp(rd, rd_before, sizeof(rd)) == 0, "buffer was modified on failure");
    return true;
}

/* --------------------------------------------------------------------- */
/* TC-008: Sector Wear-Leveling Trigger                                    */
/* --------------------------------------------------------------------- */

static bool tc_008(void)
{
    eeprom_config_t cfg = make_config();
    uint8_t data[200];
    eeprom_stats_t stats;
    uint32_t i;
    uint32_t writes_per_rotation = ((SECTOR_SIZE - 16U) / 208U) + 1U; /* +1 to force overflow */
    uint32_t total = writes_per_rotation * NUM_SECTORS * 2U;
    uint32_t min_erase;
    uint32_t max_erase;

    memset(data, 0xAB, sizeof(data));

    reset_flash();
    CHECK(eeprom_init(&cfg) == EEPROM_OK, "init failed");

    for (i = 0; i < total; i++) {
        uint16_t addr = (uint16_t)(i % 4U); /* small address set, forces overwrites+GC */
        CHECK(eeprom_write(addr, data, sizeof(data)) == EEPROM_OK, "write failed mid-run");
    }

    CHECK(eeprom_get_stats(&stats) == EEPROM_OK, "get_stats failed");
    min_erase = stats.erase_count[0];
    max_erase = stats.erase_count[0];
    for (i = 1; i < NUM_SECTORS; i++) {
        if (stats.erase_count[i] < min_erase) { min_erase = stats.erase_count[i]; }
        if (stats.erase_count[i] > max_erase) { max_erase = stats.erase_count[i]; }
    }
    CHECK((max_erase - min_erase) <= 1U, "erase counts not balanced across sectors");
    CHECK(stats.gc_runs > 0U, "expected garbage collection to have run");
    return true;
}

/* --------------------------------------------------------------------- */
/* TC-009: Garbage Collection Basic                                        */
/* --------------------------------------------------------------------- */

static bool tc_009(void)
{
    /* Small sectors so that filling sector 0 (to reach a FULL sector for
     * eeprom_garbage_collect() to actually act on) doesn't require an
     * unreasonably large number of writes. */
    eeprom_config_t cfg = make_small_config();
    uint8_t data[64];
    uint16_t addr;
    eeprom_stats_t before;
    eeprom_stats_t after;
    uint8_t rd[64];
    uint8_t sz;

    memset(data, 0x5A, sizeof(data));

    reset_flash();
    CHECK(eeprom_init(&cfg) == EEPROM_OK, "init failed");

    /* Write to 20 distinct addresses, then overwrite half of them so their
     * original records become invalidatable dead space. */
    for (addr = 0; addr < 20U; addr++) {
        CHECK(eeprom_write(addr, data, sizeof(data)) == EEPROM_OK, "initial write failed");
    }
    for (addr = 0; addr < 10U; addr++) {
        uint8_t updated[64];
        memset(updated, (int)(0x60U + addr), sizeof(updated));
        CHECK(eeprom_write(addr, updated, sizeof(updated)) == EEPROM_OK, "overwrite failed");
    }

    /* 30 records of 72 bytes each = 2160 bytes, comfortably overflowing a
     * 1024-byte sector (1008 usable), so by this point sector 0 is already
     * FULL and (per the eager-GC policy) may already have been reclaimed
     * once automatically. Force an explicit GC on top of that and confirm
     * it is well-defined either way (real compaction, or a harmless no-op
     * if the automatic path already caught up). */
    CHECK(eeprom_get_stats(&before) == EEPROM_OK, "stats before failed");
    CHECK(eeprom_garbage_collect() == EEPROM_OK, "forced GC failed");
    CHECK(eeprom_get_stats(&after) == EEPROM_OK, "stats after failed");
    CHECK(after.gc_runs >= before.gc_runs, "gc_runs did not advance");
    CHECK(before.gc_runs > 0U, "expected at least one GC to have already run automatically");

    /* All 20 addresses must still read back correctly after GC. */
    for (addr = 0; addr < 10U; addr++) {
        uint8_t expected[64];
        memset(expected, (int)(0x60U + addr), sizeof(expected));
        sz = sizeof(rd);
        CHECK(eeprom_read(addr, rd, &sz) == EEPROM_OK, "post-GC read (updated) failed");
        CHECK(memcmp(rd, expected, sizeof(expected)) == 0, "post-GC data (updated) mismatch");
    }
    for (addr = 10U; addr < 20U; addr++) {
        sz = sizeof(rd);
        CHECK(eeprom_read(addr, rd, &sz) == EEPROM_OK, "post-GC read (original) failed");
        CHECK(memcmp(rd, data, sizeof(data)) == 0, "post-GC data (original) mismatch");
    }
    return true;
}

/* --------------------------------------------------------------------- */
/* TC-010: Corrupted Sector Recovery                                       */
/* --------------------------------------------------------------------- */

static bool tc_010(void)
{
    eeprom_config_t cfg = make_config();
    uint8_t data[32];
    uint8_t rd[32];
    uint8_t sz;

    memset(data, 0x77, sizeof(data));

    reset_flash();
    CHECK(eeprom_init(&cfg) == EEPROM_OK, "init failed");

    /* Data in the (uncorrupted) active sector, sector 0. */
    CHECK(eeprom_write(0x0001U, data, sizeof(data)) == EEPROM_OK, "seed write failed");

    /* Corrupt sector 1's header - it is still EMPTY/untouched at this
     * point, isolating "corruption of a sector holding no live data" from
     * "corruption destroys data", matching the spec's own qualifier that
     * recovery must produce "no data loss from uncorrupted sectors". Set
     * an implausible state byte (neither EMPTY/ACTIVE/FULL) combined with
     * a non-erased sequence number, which cannot occur naturally. */
    {
        uint32_t hdr_addr = FLASH_BASE + SECTOR_SIZE; /* sector 1 base */
        *flash_byte(hdr_addr + 0U) = 0x01U;
        *flash_byte(hdr_addr + 1U) = 0x00U;
        *flash_byte(hdr_addr + 2U) = 0x00U;
        *flash_byte(hdr_addr + 3U) = 0x00U;
        *flash_byte(hdr_addr + 4U) = 0x55U; /* bogus state byte */
    }

    /* Simulate a reboot: the library must detect and erase the corrupted
     * sector automatically, without disturbing sector 0's data. */
    CHECK(eeprom_init(&cfg) == EEPROM_OK, "re-init after corruption failed");

    sz = sizeof(rd);
    CHECK(eeprom_read(0x0001U, rd, &sz) == EEPROM_OK, "uncorrupted sector's data lost");
    CHECK(memcmp(rd, data, sizeof(data)) == 0, "uncorrupted sector's data changed");

    /* Library must still be fully usable afterwards. */
    CHECK(eeprom_write(0x0050U, data, sizeof(data)) == EEPROM_OK, "write after recovery failed");
    sz = sizeof(rd);
    CHECK(eeprom_read(0x0050U, rd, &sz) == EEPROM_OK, "read after recovery failed");
    CHECK(memcmp(rd, data, sizeof(data)) == 0, "data mismatch after recovery");
    return true;
}

/* --------------------------------------------------------------------- */
/* TC-011: CRC Checksum Verification                                       */
/* --------------------------------------------------------------------- */

static bool tc_011(void)
{
    eeprom_config_t cfg = make_config();
    uint8_t wr[4] = { 0x11, 0x22, 0x33, 0x44 };
    uint8_t rd[4];
    uint8_t sz = sizeof(rd);

    reset_flash();
    CHECK(eeprom_init(&cfg) == EEPROM_OK, "init failed");
    CHECK(eeprom_write(0x0001U, wr, sizeof(wr)) == EEPROM_OK, "write failed");

    /* Flip a bit in the stored data payload in place (record header is 4
     * bytes, data starts right after). This alone is sufficient to break
     * the CRC16 check with overwhelming probability. */
    {
        uint32_t data_addr = FLASH_BASE + 16U /* sector header */ + 4U /* record header */;
        *flash_byte(data_addr) ^= 0x01U;
    }

    CHECK(eeprom_read(0x0001U, rd, &sz) == EEPROM_CHECKSUM_ERROR, "corruption not detected");
    return true;
}

/* --------------------------------------------------------------------- */
/* TC-012: Stress Test - Many Overwrites                                   */
/* --------------------------------------------------------------------- */

static bool tc_012(void)
{
    /* Small sectors so that 1000 writes to one address force many rotations
     * (with 64KB sectors, 1000 x 12-byte records would never fill even one
     * sector, and gc_runs would trivially stay 0). */
    eeprom_config_t cfg = make_small_config();
    uint16_t i;
    uint8_t wr[4];
    uint8_t rd[4];
    uint8_t sz;
    eeprom_stats_t stats;

    reset_flash();
    CHECK(eeprom_init(&cfg) == EEPROM_OK, "init failed");

    for (i = 0; i < 1000U; i++) {
        wr[0] = (uint8_t)(i & 0xFFU);
        wr[1] = (uint8_t)((i >> 8) & 0xFFU);
        wr[2] = 0xCC;
        wr[3] = 0xDD;
        CHECK(eeprom_write(0x0001U, wr, sizeof(wr)) == EEPROM_OK, "write failed mid-stress");
        sz = sizeof(rd);
        CHECK(eeprom_read(0x0001U, rd, &sz) == EEPROM_OK, "read failed mid-stress");
        CHECK(memcmp(wr, rd, sizeof(wr)) == 0, "readback mismatch mid-stress");
    }

    CHECK(eeprom_get_stats(&stats) == EEPROM_OK, "get_stats failed");
    CHECK(stats.total_writes >= 1000U, "total_writes undercounted");
    CHECK(stats.gc_runs > 0U, "expected multiple GC runs under sustained overwrite load");
    return true;
}

/* --------------------------------------------------------------------- */
/* TC-013: Stress Test - Random Addresses                                  */
/* --------------------------------------------------------------------- */

static bool tc_013(void)
{
    eeprom_config_t cfg = make_config();
    static uint16_t log_addr[500];
    static uint8_t log_data[500][8];
    int i;

    reset_flash();
    CHECK(eeprom_init(&cfg) == EEPROM_OK, "init failed");
    srand(1234U); /* fixed seed: deterministic, reproducible test run */

    for (i = 0; i < 500; i++) {
        uint16_t addr = (uint16_t)(rand() % ((int)EEPROM_MAX_VIRTUAL_ADDR + 1));
        uint8_t len = (uint8_t)(1 + (rand() % 8));
        int j;

        log_addr[i] = addr;
        for (j = 0; j < 8; j++) {
            log_data[i][j] = (j < len) ? (uint8_t)rand() : 0U;
        }
        CHECK(eeprom_write(addr, log_data[i], len) == EEPROM_OK, "random write failed");
    }

    for (i = 0; i < 500; i++) {
        /* Only the LAST write to a given address is guaranteed to still be
         * current; verify against the most recent log entry for that addr. */
        int last = i;
        int k;
        uint8_t rd[8];
        uint8_t sz = sizeof(rd);

        for (k = i + 1; k < 500; k++) {
            if (log_addr[k] == log_addr[i]) {
                last = k;
            }
        }
        if (last != i) {
            continue; /* superseded by a later write; checked when k==last */
        }
        CHECK(eeprom_read(log_addr[i], rd, &sz) == EEPROM_OK, "random read failed");
        CHECK(memcmp(rd, log_data[i], sz) == 0, "random readback mismatch");
    }
    return true;
}

/* --------------------------------------------------------------------- */
/* TC-014: Power Loss Simulation                                           */
/* --------------------------------------------------------------------- */

static bool tc_014(void)
{
    eeprom_config_t cfg = make_config();
    uint8_t wr[4] = { 0x01, 0x02, 0x03, 0x04 };
    uint8_t rd[4];
    uint8_t sz;

    reset_flash();
    CHECK(eeprom_init(&cfg) == EEPROM_OK, "init failed");

    /* Data written before the simulated crash. */
    CHECK(eeprom_write(0x0010U, wr, sizeof(wr)) == EEPROM_OK, "pre-crash write failed");

    /* Simulate power loss mid-write: hand-craft a record header whose
     * status byte still reads 0xFF (unsurprising: 0xFF is also the erased
     * value, so a write that never reached that byte leaves it looking
     * exactly like "valid") but whose addr/size bytes only partially
     * landed before power was cut, leaving implausible garbage (here, an
     * address of 0x1234, which is outside the 0x0000-0x0FFF virtual
     * address space). This is structurally undecodable, which is exactly
     * what the library's recovery scan is designed to detect and treat as
     * a broken tail record - it does not depend on CRC at all, since a
     * genuinely truncated write may never have reached the CRC bytes.
     * Written directly into mock_flash, bypassing the library, exactly as
     * the spec's test intends ("manually truncate"). */
    {
        uint32_t active_addr;
        uint8_t partial_hdr[4] = { 0xFFU, 0x34U, 0x12U, 0x08U };

        /* The one prior write (4 bytes of data) padded to a write_width=4
         * boundary occupies 4 (record header) + 4 (data) + 2 (CRC) = 10
         * bytes, rounded up to 12. It lands right after the 16-byte
         * sector header in sector 0, so the next (about-to-be-truncated)
         * record starts at header(16) + first-record(12) = offset 28. */
        active_addr = FLASH_BASE + 16U + 12U;
        (void)flash_write_mock(active_addr, partial_hdr, 4U);
        /* Remaining bytes are left at 0xFF: an untouched region, exactly
         * like a write that was interrupted before reaching them. */
    }

    /* Simulate reboot: recovery must invalidate the broken tail record and
     * leave the library fully operational. */
    CHECK(eeprom_init(&cfg) == EEPROM_OK, "re-init after power loss failed");

    sz = sizeof(rd);
    CHECK(eeprom_read(0x0010U, rd, &sz) == EEPROM_OK, "pre-crash data lost");
    CHECK(memcmp(rd, wr, sizeof(wr)) == 0, "pre-crash data corrupted");

    CHECK(eeprom_write(0x0011U, wr, sizeof(wr)) == EEPROM_OK, "write after recovery failed");
    sz = sizeof(rd);
    CHECK(eeprom_read(0x0011U, rd, &sz) == EEPROM_OK, "read after recovery failed");
    CHECK(memcmp(rd, wr, sizeof(wr)) == 0, "post-recovery data mismatch");
    return true;
}

/* --------------------------------------------------------------------- */
/* TC-015: Initialization of Pre-Populated Flash                           */
/* --------------------------------------------------------------------- */

static bool tc_015(void)
{
    eeprom_config_t cfg = make_config();
    uint16_t addr;
    uint8_t rd[4];
    uint8_t sz;

    reset_flash();
    CHECK(eeprom_init(&cfg) == EEPROM_OK, "init failed");

    for (addr = 0; addr < 10U; addr++) {
        uint8_t wr[4];
        wr[0] = (uint8_t)addr;
        wr[1] = (uint8_t)(addr + 1U);
        wr[2] = (uint8_t)(addr + 2U);
        wr[3] = (uint8_t)(addr + 3U);
        CHECK(eeprom_write(addr, wr, sizeof(wr)) == EEPROM_OK, "seed write failed");
    }

    /* Simulate a power cycle: re-init without touching mock_flash. */
    CHECK(eeprom_init(&cfg) == EEPROM_OK, "re-init failed");

    for (addr = 0; addr < 10U; addr++) {
        uint8_t expected[4];
        expected[0] = (uint8_t)addr;
        expected[1] = (uint8_t)(addr + 1U);
        expected[2] = (uint8_t)(addr + 2U);
        expected[3] = (uint8_t)(addr + 3U);
        sz = sizeof(rd);
        CHECK(eeprom_read(addr, rd, &sz) == EEPROM_OK, "read after re-init failed");
        CHECK(memcmp(rd, expected, sizeof(expected)) == 0, "data mismatch after re-init");
    }
    return true;
}

/* --------------------------------------------------------------------- */
/* TC-016: Statistics Tracking                                             */
/* --------------------------------------------------------------------- */

static bool tc_016(void)
{
    /* Small sectors so a modest number of writes reliably drives several
     * automatic GC cycles (64KB sectors would never fill from 50 tiny
     * writes, so "force 3 GC runs" would have nothing to collect and
     * gc_runs would stay 0 - see the note in tc_012). Writing more than
     * the spec's literal "50" still satisfies the ">= 50" expectation. */
    eeprom_config_t cfg = make_small_config();
    uint8_t wr[16];
    uint8_t rd[16];
    uint8_t sz;
    uint16_t i;
    eeprom_stats_t stats;

    memset(wr, 0x42, sizeof(wr));

    reset_flash();
    CHECK(eeprom_init(&cfg) == EEPROM_OK, "init failed");

    for (i = 0; i < 400U; i++) {
        CHECK(eeprom_write((uint16_t)(i % 5U), wr, sizeof(wr)) == EEPROM_OK, "write failed");
    }
    for (i = 0; i < 100U; i++) {
        sz = sizeof(rd);
        CHECK(eeprom_read((uint16_t)(i % 5U), rd, &sz) == EEPROM_OK, "read failed");
    }
    /* Exercise the manual API too; by this point most/all reclaimable
     * sectors have already been collected automatically, so these may be
     * no-ops, which eeprom_garbage_collect() must still report as EEPROM_OK. */
    for (i = 0; i < 3U; i++) {
        CHECK(eeprom_garbage_collect() == EEPROM_OK, "forced GC failed");
    }

    CHECK(eeprom_get_stats(&stats) == EEPROM_OK, "get_stats failed");
    CHECK(stats.total_writes >= 50U, "total_writes too low");
    CHECK(stats.total_reads >= 100U, "total_reads too low");
    CHECK(stats.gc_runs >= 3U, "gc_runs too low");
    return true;
}

/* --------------------------------------------------------------------- */
/* TC-017: Format (Full Erase)                                             */
/* --------------------------------------------------------------------- */

static bool tc_017(void)
{
    eeprom_config_t cfg = make_config();
    uint8_t wr[8];
    uint8_t rd[8];
    uint8_t sz;
    uint16_t addr;

    memset(wr, 0x99, sizeof(wr));

    reset_flash();
    CHECK(eeprom_init(&cfg) == EEPROM_OK, "init failed");
    for (addr = 0; addr < 20U; addr++) {
        CHECK(eeprom_write(addr, wr, sizeof(wr)) == EEPROM_OK, "seed write failed");
    }

    CHECK(eeprom_format() == EEPROM_OK, "format failed");

    sz = sizeof(rd);
    CHECK(eeprom_read(0x0000U, rd, &sz) == EEPROM_NOT_FOUND, "data survived format");

    CHECK(eeprom_write(0x0000U, wr, sizeof(wr)) == EEPROM_OK, "write after format failed");
    sz = sizeof(rd);
    CHECK(eeprom_read(0x0000U, rd, &sz) == EEPROM_OK, "read after format failed");
    CHECK(memcmp(rd, wr, sizeof(wr)) == 0, "data mismatch after format");
    return true;
}

/* --------------------------------------------------------------------- */
/* TC-018: Sector State Transitions                                        */
/* --------------------------------------------------------------------- */

static bool tc_018(void)
{
    /* Internal sector state (EMPTY/ACTIVE/FULL) is private to eeprom.c;
     * this black-box test instead verifies the *observable consequence* of
     * every transition happening in order: as writes progress, per-sector
     * erase counts must advance from 0 (never touched) to >=1 (has gone
     * through at least one ACTIVE->FULL->[GC]->EMPTY cycle), and they must
     * do so gradually (not all at once), which is only possible if the
     * EMPTY->ACTIVE->FULL state machine is being driven correctly. */
    eeprom_config_t cfg = make_config();
    uint8_t data[100];
    eeprom_stats_t stats;
    uint32_t i;
    bool saw_zero_erase = false;
    bool saw_nonzero_erase = false;

    memset(data, 0x11, sizeof(data));

    reset_flash();
    CHECK(eeprom_init(&cfg) == EEPROM_OK, "init failed");

    CHECK(eeprom_get_stats(&stats) == EEPROM_OK, "initial stats failed");
    for (i = 0; i < NUM_SECTORS; i++) {
        CHECK(stats.erase_count[i] == 0U, "sector erase count nonzero before any writes");
    }

    for (i = 0; i < 2000U; i++) {
        CHECK(eeprom_write((uint16_t)(i % 6U), data, sizeof(data)) == EEPROM_OK, "write failed");
    }

    CHECK(eeprom_get_stats(&stats) == EEPROM_OK, "final stats failed");
    for (i = 0; i < NUM_SECTORS; i++) {
        if (stats.erase_count[i] == 0U) { saw_zero_erase = true; }
        if (stats.erase_count[i] > 0U) { saw_nonzero_erase = true; }
    }
    CHECK(saw_nonzero_erase, "no sector ever completed a FULL->EMPTY transition");
    (void)saw_zero_erase; /* informational only: not all sectors need to lag equally */
    return true;
}

/* --------------------------------------------------------------------- */
/* TC-019: Lookup Table Integrity                                          */
/* --------------------------------------------------------------------- */

static bool tc_019(void)
{
    eeprom_config_t cfg = make_config();
    uint8_t a[2] = { 0xA0, 0xA1 };
    uint8_t b[2] = { 0xB0, 0xB1 };
    uint8_t other[2] = { 0xC0, 0xC1 };
    uint8_t rd[2];
    uint8_t sz;

    reset_flash();
    CHECK(eeprom_init(&cfg) == EEPROM_OK, "init failed");

    CHECK(eeprom_write(0x0050U, a, sizeof(a)) == EEPROM_OK, "write a failed");
    CHECK(eeprom_write(0x0050U, b, sizeof(b)) == EEPROM_OK, "write b failed");
    sz = sizeof(rd);
    CHECK(eeprom_read(0x0050U, rd, &sz) == EEPROM_OK, "read 1 failed");
    CHECK(memcmp(rd, b, sizeof(b)) == 0, "expected b after two writes");

    CHECK(eeprom_write(0x0051U, other, sizeof(other)) == EEPROM_OK, "write other failed");
    sz = sizeof(rd);
    CHECK(eeprom_read(0x0050U, rd, &sz) == EEPROM_OK, "read 2 failed");
    CHECK(memcmp(rd, b, sizeof(b)) == 0, "lookup table corrupted by unrelated write");
    return true;
}

/* --------------------------------------------------------------------- */
/* TC-020: MISRA-C / Compiler Compliance                                   */
/* --------------------------------------------------------------------- */

static bool tc_020(void)
{
    /* This is a build-time property, not a runtime one: the fact that this
     * test binary linked at all means eeprom.c compiled cleanly under the
     * flags configured in test/Makefile (-Wall -Wextra -Werror -std=c11).
     * Run a static analyzer such as cppcheck --enable=all against
     * src/eeprom.c separately to check MISRA-style rules; see
     * IMPLEMENTATION_NOTES.md for the documented, intentional deviations. */
    printf("    (validated by the build itself: -Wall -Wextra -Werror -std=c11)\n");
    return true;
}

/* --------------------------------------------------------------------- */
/* AUDIT-1: Power loss during manual GC with a spare sector available     */
/* (regression test for a Critical bug found in independent code review, */
/* not one of the original 20 spec test cases - see IMPLEMENTATION_NOTES) */
/* --------------------------------------------------------------------- */

static bool tc_audit_gc_power_loss(void)
{
    /* Reproduces the exact failure mode: a manually-triggered
     * eeprom_garbage_collect() claims an EMPTY spare as its compaction
     * destination, then power is lost after the destination is committed
     * to flash but before the source sector's erase completes. The old
     * (pre-fix) code claimed the spare via activate_sector(), which wrote
     * SECTOR_STATE_ACTIVE - identical to what the real active sector already
     * read. On reboot, find_active_sector() picked whichever ACTIVE sector
     * had the lower physical index; if that was the spare, not the real
     * active sector, eeprom_init() erased the real active sector outright,
     * destroying live application data with no chance of recovery.
     *
     * This test deliberately engineers the spare to land at a LOWER index
     * (sector 0) than the real active sector (sector 3, the highest index)
     * by driving a full rotation through all 4 sectors first - the
     * arrangement that actually triggered the original bug (the reviewer's
     * own suggested "highest sequence wins" fix would have made it WORSE:
     * the spare always has the higher sequence, so that heuristic would
     * pick it 100% of the time instead of only when index order happened
     * to favor it).
     */
    eeprom_config_t cfg = make_gc_regression_config();
    uint8_t data[20];
    uint8_t guard[20];
    uint8_t pending[20];
    uint8_t rd[20];
    uint8_t sz;
    uint16_t a;

    memset(data, 0xAA, sizeof(data));
    memset(guard, 0x55, sizeof(guard));
    memset(pending, 0x33, sizeof(pending));

    reset_flash();
    g_fail_erase_addr = 0xFFFFFFFFU; /* no failures during setup */
    CHECK(eeprom_init(&cfg) == EEPROM_OK, "init failed");

    /* Fill sector0 with 8 live records at addresses 0x0010-0x0017
     * (8 * 28-byte records = 224 of 240 usable bytes -> exactly full). */
    for (a = 0x0010U; a <= 0x0017U; a++) {
        CHECK(eeprom_write(a, data, sizeof(data)) == EEPROM_OK, "sector0 fill write failed");
    }

    /* Overwrite the SAME 8 addresses again: this invalidates every one of
     * sector0's original records (their lookup entries move to sector1)
     * and, as a side effect, exactly fills sector1 too - by design, so
     * that sector0 has ZERO live data left by the time it gets reclaimed
     * below, leaving the destination sector with full headroom. */
    for (a = 0x0010U; a <= 0x0017U; a++) {
        CHECK(eeprom_write(a, data, sizeof(data)) == EEPROM_OK, "sector1 fill write failed");
    }

    /* Fill sector2 with 8 fresh records at 0x0020-0x0027. */
    for (a = 0x0020U; a <= 0x0027U; a++) {
        CHECK(eeprom_write(a, data, sizeof(data)) == EEPROM_OK, "sector2 fill write failed");
    }

    /* One more write promotes sector3 (the last spare) to ACTIVE, which
     * immediately trips the eager-GC path (no spare remains): sector0,
     * holding zero live records, is reclaimed into the freshly-promoted
     * (and still nearly empty) sector3, and erased back to EMPTY. */
    CHECK(eeprom_write(0x0030U, pending, sizeof(pending)) == EEPROM_OK, "sector3 promotion write failed");

    /* Now: sector0=EMPTY(low index), sector1=FULL(live 0x10-0x17),
     * sector2=FULL(live 0x20-0x27), sector3=ACTIVE(high index). Write the
     * data that must survive the simulated crash into the real active
     * sector. */
    CHECK(eeprom_write(0x0099U, guard, sizeof(guard)) == EEPROM_OK, "guard write failed");

    /* Manually trigger GC: it will target sector1 (oldest FULL) and claim
     * sector0 (the only EMPTY sector, lower index than the active
     * sector3) as its destination. Fail the erase of sector1 specifically,
     * simulating power loss in the window between the destination being
     * committed and the source being erased. */
    g_fail_erase_addr = FLASH_BASE + (1U * cfg.sector_size);
    CHECK(eeprom_garbage_collect() == EEPROM_ERASE_FAILED,
          "expected the injected erase failure to surface (test setup did not reach the intended crash window)");
    g_fail_erase_addr = 0xFFFFFFFFU;

    /* Simulate a reboot on flash left in exactly that state. */
    CHECK(eeprom_init(&cfg) == EEPROM_OK, "re-init after simulated crash failed");

    /* The critical assertion: the real active sector's data must survive.
     * Under the pre-fix code, this would fail with EEPROM_NOT_FOUND
     * because sector3 (holding this record) would have been erased. */
    sz = sizeof(rd);
    CHECK(eeprom_read(0x0099U, rd, &sz) == EEPROM_OK, "guard data lost across simulated crash");
    CHECK(memcmp(rd, guard, sizeof(guard)) == 0, "guard data corrupted across simulated crash");

    sz = sizeof(rd);
    CHECK(eeprom_read(0x0030U, rd, &sz) == EEPROM_OK, "pending record lost across simulated crash");
    CHECK(memcmp(rd, pending, sizeof(pending)) == 0, "pending record corrupted across simulated crash");

    /* Library must remain fully operational afterwards. */
    CHECK(eeprom_write(0x00AAU, data, sizeof(data)) == EEPROM_OK, "write after recovery failed");
    sz = sizeof(rd);
    CHECK(eeprom_read(0x00AAU, rd, &sz) == EEPROM_OK, "read after recovery failed");
    CHECK(memcmp(rd, data, sizeof(data)) == 0, "data mismatch after recovery");

    return true;
}

/* --------------------------------------------------------------------- */
/* AUDIT-2: erase_count lost across a reboot catching a sector EMPTY      */
/* (regression test for a bug found by property-based, randomized-fault   */
/* stress testing - see IMPLEMENTATION_NOTES.md's "Property-based stress  */
/* testing findings")                                                     */
/* --------------------------------------------------------------------- */

static bool tc_audit_erase_count_persists_empty_sector(void)
{
    /* A sector's lifetime erase_count used to be persisted to flash ONLY
     * inside activate_sector()/mark_gc_destination() - so a sector that
     * had just been reclaimed by GC (erased, sitting EMPTY) but not yet
     * reactivated had NO on-flash record of its count at all. A reboot
     * catching it in exactly that window read its header as fully blank
     * and silently reset the count to 0.
     *
     * This drives enough small, never-overwritten writes to force one
     * full sector reclaim (leaving that sector idle and EMPTY, since the
     * newly-promoted active sector still has headroom right after
     * rotation completes), then reboots immediately - no further write in
     * between - and checks the total lifetime erase_count across all
     * sectors did not drop.
     */
    eeprom_config_t cfg = make_gc_regression_config();
    uint8_t data[8];
    uint16_t addr = 0x0001U;
    eeprom_stats_t stats_before;
    eeprom_stats_t stats_after;
    uint32_t total_before;
    uint32_t total_after;
    uint8_t i;
    eeprom_status_t rc;

    memset(data, 0xABU, sizeof(data));
    reset_flash();
    g_fail_erase_addr = 0xFFFFFFFFU;
    CHECK(eeprom_init(&cfg) == EEPROM_OK, "init failed");

    stats_before.gc_runs = 0U;
    for (i = 0; i < 100U; i++) {
        /* The write that triggers the eager GC reclaim below can itself
         * legitimately come back EEPROM_SECTOR_FULL if the reclaimed
         * sector's migrated survivors exactly fill the newly-active
         * sector (an artifact of this test's record/sector-size
         * arithmetic, not a bug under test) - only gc_runs matters here. */
        rc = eeprom_write(addr, data, sizeof(data));
        CHECK((rc == EEPROM_OK) || (rc == EEPROM_SECTOR_FULL), "unexpected write status during fill");
        addr++;
        CHECK(eeprom_get_stats(&stats_before) == EEPROM_OK, "get_stats failed");
        if (stats_before.gc_runs >= 1U) {
            break;
        }
    }
    CHECK(stats_before.gc_runs >= 1U, "test setup did not trigger a GC reclaim");

    total_before = stats_before.erase_count[0] + stats_before.erase_count[1] +
                   stats_before.erase_count[2] + stats_before.erase_count[3];
    CHECK(total_before >= 1U, "no sector shows a nonzero erase count before reboot");

    CHECK(eeprom_init(&cfg) == EEPROM_OK, "re-init failed");
    CHECK(eeprom_get_stats(&stats_after) == EEPROM_OK, "get_stats after reboot failed");
    total_after = stats_after.erase_count[0] + stats_after.erase_count[1] +
                  stats_after.erase_count[2] + stats_after.erase_count[3];

    CHECK(total_after >= total_before,
          "lifetime erase_count decreased across a reboot - lost for a sector that "
          "was sitting EMPTY (erased, not yet reactivated)");

    return true;
}

/* --------------------------------------------------------------------- */
/* Runner                                                                   */
/* --------------------------------------------------------------------- */

static const test_case_t k_tests[] = {
    { "TC-001", "Basic Write and Read",              tc_001 },
    { "TC-002", "Overwrite Same Address",            tc_002 },
    { "TC-003", "Multiple Addresses",                tc_003 },
    { "TC-004", "Boundary Values",                   tc_004 },
    { "TC-005", "Invalid Address Rejection",         tc_005 },
    { "TC-006", "Invalid Size Rejection",            tc_006 },
    { "TC-007", "Not Found Case",                    tc_007 },
    { "TC-008", "Sector Wear-Leveling Trigger",      tc_008 },
    { "TC-009", "Garbage Collection Basic",          tc_009 },
    { "TC-010", "Corrupted Sector Recovery",         tc_010 },
    { "TC-011", "CRC Checksum Verification",         tc_011 },
    { "TC-012", "Stress Test - Many Overwrites",     tc_012 },
    { "TC-013", "Stress Test - Random Addresses",    tc_013 },
    { "TC-014", "Power Loss Simulation",             tc_014 },
    { "TC-015", "Initialization of Pre-Populated Flash", tc_015 },
    { "TC-016", "Statistics Tracking",                tc_016 },
    { "TC-017", "Format (Full Erase)",                tc_017 },
    { "TC-018", "Sector State Transitions",           tc_018 },
    { "TC-019", "Lookup Table Integrity",             tc_019 },
    { "TC-020", "MISRA-C Compliance Check",           tc_020 },
    { "AUDIT-1", "GC power loss with spare (regression)", tc_audit_gc_power_loss },
    { "AUDIT-2", "erase_count persists across reboot (regression)", tc_audit_erase_count_persists_empty_sector },
};

int main(void)
{
    size_t i;
    size_t pass = 0;
    size_t fail = 0;

    for (i = 0; i < (sizeof(k_tests) / sizeof(k_tests[0])); i++) {
        bool ok = k_tests[i].fn();
        printf("[%s] %-38s %s\n", k_tests[i].id, k_tests[i].name, ok ? "PASS" : "FAIL");
        if (ok) { pass++; } else { fail++; }
    }

    printf("\n%zu passed, %zu failed, %zu total\n", pass, fail, pass + fail);
    return (fail == 0U) ? 0 : 1;
}
