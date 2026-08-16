/**
 * @file    eeprom.c
 * @brief   STM32 EEPROM emulation library - implementation.
 *
 * On-disk layout
 * ---------------
 * Each managed Flash sector begins with a fixed-size, write-width-aligned
 * header, followed by a log of variable-length records:
 *
 *   Sector header (SECTOR_HEADER_SIZE = 16 bytes):
 *     offset 0..3   sequence     uint32_t, little-endian, assigned when the
 *                                sector is promoted to ACTIVE, monotonically
 *                                increasing across the library's lifetime.
 *     offset 4      state        uint8_t: 0xFF=EMPTY, 0xFE=ACTIVE, 0xFC=FULL.
 *                                Encoded so every transition only clears
 *                                bits (EMPTY -> ACTIVE -> FULL), which is
 *                                achievable with a single Flash program
 *                                operation and no erase.
 *     offset 5..7   reserved     0xFF.
 *     offset 8..11  erase_count  uint32_t, little-endian, lifetime erase
 *                                count for this physical sector.
 *     offset 12..15 reserved     0xFF.
 *
 *   Record (RECORD_HEADER_SIZE = 4 byte header + data + 2 byte CRC16,
 *   padded with 0xFF to a write_width boundary):
 *     offset 0      status       0xFF = valid, 0x00 = invalidated/deleted.
 *     offset 1..2   addr         uint16_t, little-endian, virtual address
 *                                (extended from the 1-byte field described
 *                                in the spec's example table, since this
 *                                library's 0x0000-0x0FFF address space
 *                                needs more than 8 bits - the spec itself
 *                                calls this out as the expected extension).
 *     offset 3      size         uint8_t, 1-255.
 *     offset 4..    data         `size` bytes.
 *     ..+2          crc16        CRC16-CCITT (poly 0x1021, init 0x0000)
 *                                over [addr_lo, addr_hi, size, data...].
 *     ..padding     0xFF fill up to the next write_width boundary.
 *
 * The record's on-flash location is never reused: a value at a given
 * virtual address is always addressed by the *newest* record for that
 * address, tracked in a RAM lookup table indexed directly by virtual
 * address (`g_lookup`). Superseded records are best-effort invalidated in
 * place (status byte cleared to 0x00) but this is not load-bearing for
 * correctness - the lookup table, rebuilt at init purely by scanning
 * records in flash order, is always the source of truth for "which record
 * is current" (see scan_sector_records() and compact_sector()).
 *
 * Sector free space is intentionally NOT persisted as a running write
 * pointer in the header: under the AND-only Flash write semantics this
 * format relies on (a byte can only have bits cleared, never set, without
 * a full sector erase), a monotonically-growing offset cannot be safely
 * rewritten on every record append. Instead, the write offset is always
 * recovered by scanning forward from the sector header until the first
 * fully-erased (0xFF 0xFF 0xFF 0xFF) record slot is found. This also
 * naturally implements corruption/partial-write detection: a record
 * header cannot legally read as all-0xFF (the virtual address's high byte
 * is always <= 0x0F, since EEPROM_MAX_VIRTUAL_ADDR is 0x0FFF), so an
 * all-0xFF slot unambiguously means "free space starts here".
 */

#include "eeprom.h"

#include <string.h>

/* --------------------------------------------------------------------- */
/* Private constants                                                      */
/* --------------------------------------------------------------------- */

#define SECTOR_HEADER_SIZE   ((uint32_t)16U)

/* Aliases of the public EEPROM_SECTOR_STATE_* constants (inc/eeprom.h),
 * not a second definition - see that header for the value/rationale
 * (including why GC_DEST clears a bit independent of the ACTIVE/FULL
 * chain) and eeprom_get_stats() for where these become visible to
 * callers via eeprom_stats_t.sector_state[]. */
#define SECTOR_STATE_EMPTY   EEPROM_SECTOR_STATE_EMPTY
#define SECTOR_STATE_ACTIVE  EEPROM_SECTOR_STATE_ACTIVE
#define SECTOR_STATE_FULL    EEPROM_SECTOR_STATE_FULL
#define SECTOR_STATE_GC_DEST EEPROM_SECTOR_STATE_GC_DEST

/* The free-space scan in scan_sector_records() treats a fully-erased
 * record header (0xFF 0xFF 0xFF 0xFF) as unambiguous proof that no record
 * was ever written there. That's only true if a real record's address
 * high byte can never be 0xFF, i.e. EEPROM_MAX_VIRTUAL_ADDR must stay
 * below 0xFF00 even if an integrator overrides it to shrink g_lookup. */
#if EEPROM_MAX_VIRTUAL_ADDR >= 0xFF00U
#error "EEPROM_MAX_VIRTUAL_ADDR must be < 0xFF00 (see scan_sector_records)"
#endif

#define RECORD_HEADER_SIZE   ((uint16_t)4U)
#define RECORD_CRC_SIZE      ((uint16_t)2U)
#define RECORD_MAX_DATA_SIZE ((uint16_t)255U)
/* RECORD_HEADER_SIZE + RECORD_MAX_DATA_SIZE + RECORD_CRC_SIZE, rounded up
 * to the largest supported write_width (8). */
#define RECORD_MAX_PADDED_SIZE ((uint16_t)264U)

#define RECORD_STATUS_VALID   ((uint8_t)0xFFU)
#define RECORD_STATUS_INVALID ((uint8_t)0x00U)

/* Bounded retry count for a record-header read that fails during recovery
 * scanning (scan_sector_records()). A flash_read() failure means the I/O
 * attempt itself faulted, not that the underlying data is bad (see
 * validate_record()'s doc comment on that same distinction) - a single
 * failed attempt is not evidence the sector's tail was cut off by a power
 * loss, so a transient glitch is given a bounded chance to clear before
 * that conclusion is drawn. */
#define SCAN_HEADER_READ_RETRIES ((uint8_t)3U)

#define LOOKUP_EMPTY          ((uint32_t)0xFFFFFFFFU)
#define SEQUENCE_ERASED_MARK  ((uint32_t)0xFFFFFFFFU)

#define EEPROM_VIRTUAL_SIZE   ((uint32_t)EEPROM_MAX_VIRTUAL_ADDR + 1U)

/* --------------------------------------------------------------------- */
/* Private types                                                          */
/* --------------------------------------------------------------------- */

typedef struct {
    uint32_t sequence;
    uint8_t  state;
    uint32_t erase_count;
    uint32_t write_offset; /* bytes from sector base to first free byte */
} sector_meta_t;

/* --------------------------------------------------------------------- */
/* Private variables                                                      */
/* --------------------------------------------------------------------- */

static eeprom_config_t g_config;
static sector_meta_t   g_sectors[EEPROM_MAX_SECTORS];
static uint32_t        g_lookup[EEPROM_VIRTUAL_SIZE];
static int8_t           g_active_sector;
static uint32_t         g_next_sequence;
static bool             g_initialized;
static eeprom_stats_t   g_stats;

/* Audit fix (Claim 3): write_record()'s and compact_sector()'s working
 * buffers moved off the stack. Both functions sit on the write/GC call
 * chain (eeprom_write -> write_record -> ensure_room ->
 * perform_garbage_collection -> compact_sector), which the header
 * documents as NOT ISR-reentrant and requiring external serialization
 * (single active writer) - so sharing these as static, mutable-but-never-
 * concurrently-touched buffers is safe under that existing contract, and
 * removes ~800 bytes of worst-case stack depth from that one call chain
 * on top of each function's own small locals. validate_record()'s
 * buffers are deliberately NOT moved here: it is also called from
 * eeprom_read(), which IS documented ISR-safe/reentrant-with-writes, so
 * giving it a static buffer would let an ISR-context read race a
 * main-thread write's GC pass over the same memory - keeping it on the
 * stack is the correct choice there, not an oversight. */
static uint8_t g_write_record_buf[RECORD_MAX_PADDED_SIZE];
static uint8_t g_write_record_crc_input[3U + RECORD_MAX_DATA_SIZE];
static uint8_t g_compact_sector_buf[RECORD_MAX_PADDED_SIZE];

/* --------------------------------------------------------------------- */
/* Small serialization helpers (explicit little-endian on-flash layout,   */
/* independent of host/target endianness)                                 */
/* --------------------------------------------------------------------- */

static uint32_t load_u32(const uint8_t *p)
{
    return ((uint32_t)p[0]) | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void store_u32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value & 0xFFU);
    p[1] = (uint8_t)((value >> 8) & 0xFFU);
    p[2] = (uint8_t)((value >> 16) & 0xFFU);
    p[3] = (uint8_t)((value >> 24) & 0xFFU);
}

static uint16_t load_u16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0]) | ((uint16_t)((uint16_t)p[1] << 8)));
}

static void store_u16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value & 0xFFU);
    p[1] = (uint8_t)((value >> 8) & 0xFFU);
}

/* --------------------------------------------------------------------- */
/* CRC16-CCITT (poly 0x1021, init 0x0000)                                 */
/* --------------------------------------------------------------------- */

static uint16_t crc16_ccitt(const uint8_t *data, uint16_t len)
{
    uint16_t crc = 0x0000U;
    uint16_t i;

    for (i = 0; i < len; i++) {
        uint8_t bit;
        crc = (uint16_t)(crc ^ (uint16_t)((uint16_t)data[i] << 8));
        for (bit = 0; bit < 8U; bit++) {
            if ((crc & 0x8000U) != 0U) {
                crc = (uint16_t)((uint16_t)(crc << 1) ^ 0x1021U);
            } else {
                crc = (uint16_t)(crc << 1);
            }
        }
    }
    return crc;
}

/* --------------------------------------------------------------------- */
/* Sector management                                                      */
/* --------------------------------------------------------------------- */

static uint32_t get_sector_address(uint8_t sector_index)
{
    return g_config.flash_start + ((uint32_t)sector_index * g_config.sector_size);
}

static uint16_t align_up_u16(uint16_t value, uint8_t width)
{
    uint16_t rem = (uint16_t)(value % (uint16_t)width);

    if (rem == 0U) {
        return value;
    }
    return (uint16_t)(value + ((uint16_t)width - rem));
}

static uint16_t record_padded_size(uint8_t size)
{
    uint16_t raw = (uint16_t)((uint16_t)RECORD_HEADER_SIZE + (uint16_t)size + RECORD_CRC_SIZE);
    return align_up_u16(raw, g_config.write_width);
}

static int read_sector_header(uint8_t sector_index, uint32_t *sequence,
                               uint8_t *state, uint32_t *erase_count)
{
    uint8_t buf[16];
    uint8_t attempt;
    int read_rc = -1;

    /* Same bounded-retry rationale as scan_sector_records()'s record
     * header read: a flash_read() failure here means the I/O attempt
     * faulted, not that the sector header itself is bad. Without this,
     * scan_all_sectors() treats a single transient glitch identically to
     * a genuinely corrupt header and destructively erases the sector -
     * for the ACTIVE sector, that means real, uncorrupted application
     * data. Audit fix, found the same way as the record-header case:
     * property-based, randomized-reboot stress testing that injected
     * standalone read failures with no data corruption at all. */
    for (attempt = 0; attempt < SCAN_HEADER_READ_RETRIES; attempt++) {
        read_rc = g_config.flash_read(get_sector_address(sector_index), buf, (uint16_t)sizeof(buf));
        if (read_rc == 0) {
            break;
        }
    }
    if (read_rc != 0) {
        return -1;
    }
    *sequence = load_u32(&buf[0]);
    *state = buf[4];
    *erase_count = load_u32(&buf[8]);
    return 0;
}

static int write_sector_header(uint8_t sector_index, uint32_t sequence,
                                uint8_t state, uint32_t erase_count)
{
    uint8_t buf[16];

    store_u32(&buf[0], sequence);
    buf[4] = state;
    buf[5] = 0xFFU;
    buf[6] = 0xFFU;
    buf[7] = 0xFFU;
    store_u32(&buf[8], erase_count);
    buf[12] = 0xFFU;
    buf[13] = 0xFFU;
    buf[14] = 0xFFU;
    buf[15] = 0xFFU;

    if (g_config.flash_write(get_sector_address(sector_index), buf, (uint16_t)sizeof(buf)) != 0) {
        return -1;
    }
    return 0;
}

/**
 * Best-effort persistence of a just-erased sector's lifetime erase_count,
 * called immediately after every transition to SECTOR_STATE_EMPTY.
 *
 * Audit fix: without this, erase_count is only ever written to flash
 * inside activate_sector()/mark_gc_destination() - so a sector that has
 * been erased (via GC, the eeprom_init() extra-ACTIVE recovery path, or
 * eeprom_format()) but has not yet been reactivated has NO on-flash
 * record of its erase count at all: read_sector_header() sees a fully
 * blank (all-0xFF) header, and scan_all_sectors() - correctly, for a
 * sector that has genuinely never been written - treats that as
 * erase_count 0. A reboot that catches a sector in exactly that window
 * silently loses its true lifetime count, breaking the "lifetime"
 * guarantee eeprom_stats_t.erase_count documents and, transitively, any
 * wear-leveling health check built on it. Found via randomized-reboot
 * property-based stress testing.
 *
 * Deliberately passes SEQUENCE_ERASED_MARK for the sequence field rather
 * than 0: under this format's AND-only write semantics a byte can only
 * have bits cleared, never set, so writing 0 here would permanently trap
 * that field at 0 - a later activate_sector()/mark_gc_destination() call
 * would never be able to AND-narrow it back up to a real, non-zero
 * sequence number. SEQUENCE_ERASED_MARK (0xFFFFFFFF) is what the field
 * already reads as immediately after an erase, so writing it back is a
 * true no-op there, leaving it free for a later real value while still
 * durably recording erase_count. Best-effort (return value ignored) for
 * the same reason invalidate_record() is: on failure, behavior simply
 * reverts to the pre-fix state for this one sector, not a new regression.
 */
static void persist_empty_sector_erase_count(uint8_t sector_index)
{
    (void)write_sector_header(sector_index, SEQUENCE_ERASED_MARK, SECTOR_STATE_EMPTY,
                               g_sectors[sector_index].erase_count);
}

static int8_t find_empty_sector(void)
{
    uint8_t i;

    for (i = 0; i < g_config.num_sectors; i++) {
        if (g_sectors[i].state == SECTOR_STATE_EMPTY) {
            return (int8_t)i;
        }
    }
    return -1;
}

static int8_t find_active_sector(void)
{
    uint8_t i;

    for (i = 0; i < g_config.num_sectors; i++) {
        if (g_sectors[i].state == SECTOR_STATE_ACTIVE) {
            return (int8_t)i;
        }
    }
    return -1;
}

/**
 * Oldest reclaimable sector: the lowest-sequence sector currently in
 * either FULL (normal end-of-rotation) or GC_DEST (a prior GC's
 * compaction destination, itself now eligible to be reclaimed in turn) -
 * both states mean "closed to new writes, may hold a mix of live and dead
 * records."
 */
static int8_t find_gc_target_sector(void)
{
    uint8_t i;
    int8_t best = -1;

    for (i = 0; i < g_config.num_sectors; i++) {
        if ((g_sectors[i].state == SECTOR_STATE_FULL) || (g_sectors[i].state == SECTOR_STATE_GC_DEST)) {
            if ((best < 0) || (g_sectors[i].sequence < g_sectors[(uint8_t)best].sequence)) {
                best = (int8_t)i;
            }
        }
    }
    return best;
}

/**
 * Promote an EMPTY sector to ACTIVE: assigns it the next sequence number,
 * persists the header, and resets its in-RAM write offset. This is the
 * ONLY function that ever writes SECTOR_STATE_ACTIVE - it must be used
 * exclusively for "the sector application writes now go to." A GC
 * compaction destination must use mark_gc_destination() instead, even
 * though the two functions are otherwise identical, precisely so that at
 * most one sector can ever read back as ACTIVE (see mark_gc_destination()).
 */
static eeprom_status_t activate_sector(uint8_t sector_index)
{
    if (write_sector_header(sector_index, g_next_sequence, SECTOR_STATE_ACTIVE,
                             g_sectors[sector_index].erase_count) != 0) {
        return EEPROM_WRITE_FAILED;
    }
    g_sectors[sector_index].sequence = g_next_sequence;
    g_sectors[sector_index].state = SECTOR_STATE_ACTIVE;
    g_sectors[sector_index].write_offset = SECTOR_HEADER_SIZE;
    g_next_sequence += 1U;
    return EEPROM_OK;
}

/**
 * Claim an EMPTY sector as a garbage-collection compaction destination.
 * Deliberately writes SECTOR_STATE_GC_DEST, not SECTOR_STATE_ACTIVE: this
 * sector never receives new application writes (g_active_sector is left
 * untouched by the caller), only the migrated survivors of the sector
 * being reclaimed. Using a distinct state closed under find_gc_target_sector()
 * rather than find_active_sector() means that if power is lost anywhere
 * between this call and the source sector's erase, eeprom_init() can never
 * mistake this sector for the real active one, regardless of which
 * physical index or sequence number it happens to have - it is exactly as
 * crash-safe as any other GC-created FULL sector, no special recovery path
 * needed.
 */
static eeprom_status_t mark_gc_destination(uint8_t sector_index)
{
    if (write_sector_header(sector_index, g_next_sequence, SECTOR_STATE_GC_DEST,
                             g_sectors[sector_index].erase_count) != 0) {
        return EEPROM_WRITE_FAILED;
    }
    g_sectors[sector_index].sequence = g_next_sequence;
    g_sectors[sector_index].state = SECTOR_STATE_GC_DEST;
    g_sectors[sector_index].write_offset = SECTOR_HEADER_SIZE;
    g_next_sequence += 1U;
    return EEPROM_OK;
}

/* --------------------------------------------------------------------- */
/* Record operations                                                      */
/* --------------------------------------------------------------------- */

/* Forward declaration: ensure_room() eagerly reclaims space via GC, but
 * perform_garbage_collection() is defined after it (it depends on
 * compact_sector(), which in turn is simplest to express once record
 * helpers below are available). */
static eeprom_status_t perform_garbage_collection(void);

static int find_record(uint16_t addr, uint32_t *flash_addr)
{
    if (g_lookup[addr] == LOOKUP_EMPTY) {
        return -1;
    }
    *flash_addr = g_lookup[addr];
    return 0;
}

/**
 * Verifies that the record at flash_addr has a valid status byte and an
 * intact CRC16. On EEPROM_OK, *data_size holds the record's payload size.
 *
 * Distinguishes two different failure modes, which matter to callers that
 * want to report them accurately (audit fix, Claim 4): EEPROM_READ_FAILED
 * means the flash_read() callback itself reported an I/O error - the
 * stored data may be perfectly intact, only the read attempt failed, and
 * a retry might succeed. EEPROM_CHECKSUM_ERROR means the read succeeded
 * but what it returned is not a valid record (bad status byte, impossible
 * size, or CRC mismatch) - the data itself is the problem, not the read.
 */
static eeprom_status_t validate_record(uint32_t flash_addr, uint16_t *data_size)
{
    uint8_t hdr[RECORD_HEADER_SIZE];
    uint8_t body[RECORD_MAX_DATA_SIZE + RECORD_CRC_SIZE];
    uint8_t crc_input[3U + RECORD_MAX_DATA_SIZE];
    uint16_t size16;
    uint16_t stored_crc;
    uint16_t calc_crc;

    if (g_config.flash_read(flash_addr, hdr, RECORD_HEADER_SIZE) != 0) {
        return EEPROM_READ_FAILED;
    }
    if (hdr[0] != RECORD_STATUS_VALID) {
        return EEPROM_CHECKSUM_ERROR;
    }
    size16 = (uint16_t)hdr[3];
    if (size16 == 0U) {
        return EEPROM_CHECKSUM_ERROR;
    }

    if (g_config.flash_read(flash_addr + RECORD_HEADER_SIZE, body,
                             (uint16_t)(size16 + RECORD_CRC_SIZE)) != 0) {
        return EEPROM_READ_FAILED;
    }

    crc_input[0] = hdr[1];
    crc_input[1] = hdr[2];
    crc_input[2] = hdr[3];
    (void)memcpy(&crc_input[3], body, size16);
    calc_crc = crc16_ccitt(crc_input, (uint16_t)(3U + size16));
    stored_crc = load_u16(&body[size16]);

    if (calc_crc != stored_crc) {
        return EEPROM_CHECKSUM_ERROR;
    }
    *data_size = size16;
    return EEPROM_OK;
}

/**
 * Best-effort invalidation: clears the status byte of the record at
 * flash_addr to RECORD_STATUS_INVALID via a single write_width-aligned
 * Flash write (safe under AND-only write semantics; a failure here is not
 * fatal, see the file-level comment on lookup-table authority).
 */
static void invalidate_record(uint32_t flash_addr)
{
    uint8_t buf[8];
    uint8_t i;

    buf[0] = RECORD_STATUS_INVALID;
    for (i = 1; i < g_config.write_width; i++) {
        buf[i] = 0xFFU;
    }
    (void)g_config.flash_write(flash_addr, buf, g_config.write_width);
}

static eeprom_status_t ensure_room(uint16_t rec_total)
{
    uint32_t free_space;
    int8_t empty_idx;
    eeprom_status_t rc;

    if (g_active_sector < 0) {
        return EEPROM_FORMAT_ERROR;
    }

    free_space = g_config.sector_size - g_sectors[(uint8_t)g_active_sector].write_offset;
    if (free_space >= (uint32_t)rec_total) {
        return EEPROM_OK;
    }

    /* Retire the current active sector. */
    if (write_sector_header((uint8_t)g_active_sector, g_sectors[(uint8_t)g_active_sector].sequence,
                             SECTOR_STATE_FULL, g_sectors[(uint8_t)g_active_sector].erase_count) != 0) {
        return EEPROM_WRITE_FAILED;
    }
    g_sectors[(uint8_t)g_active_sector].state = SECTOR_STATE_FULL;

    empty_idx = find_empty_sector();
    if (empty_idx < 0) {
        return EEPROM_SECTOR_FULL;
    }

    rc = activate_sector((uint8_t)empty_idx);
    if (rc != EEPROM_OK) {
        return rc;
    }
    g_active_sector = empty_idx;

    /* Keep at least one spare sector available for the next rotation: if
     * promoting the sector above consumed the last spare, eagerly reclaim
     * the oldest FULL sector into the (still nearly-empty) new active
     * sector. This always fits, since a sector's own valid records can
     * never exceed that sector's capacity. */
    if (find_empty_sector() < 0) {
        (void)perform_garbage_collection();
    }

    free_space = g_config.sector_size - g_sectors[(uint8_t)g_active_sector].write_offset;
    if (free_space < (uint32_t)rec_total) {
        return EEPROM_SECTOR_FULL;
    }
    return EEPROM_OK;
}

static eeprom_status_t compact_sector(uint8_t src, uint8_t dst)
{
    uint32_t src_base = get_sector_address(src);
    uint32_t cur = SECTOR_HEADER_SIZE;
    uint8_t hdr[RECORD_HEADER_SIZE];
    uint8_t *buf = g_compact_sector_buf;

    while (cur < g_sectors[src].write_offset) {
        uint16_t addr;
        uint8_t size;
        uint16_t rec_total;
        uint32_t rec_loc = src_base + cur;

        if (g_config.flash_read(rec_loc, hdr, RECORD_HEADER_SIZE) != 0) {
            return EEPROM_WRITE_FAILED;
        }
        addr = load_u16(&hdr[1]);
        size = hdr[3];
        rec_total = record_padded_size(size);

        if ((addr <= EEPROM_MAX_VIRTUAL_ADDR) && (g_lookup[addr] == rec_loc)) {
            /* Deliberately keyed on g_lookup alone, not this record's own
             * status byte: g_lookup is this file's documented source of
             * truth for "which record is current" (see the file-level
             * doc comment), so if it already says addr's live record is
             * at rec_loc, that stands regardless of what rec_loc's own
             * header currently reads - migrate it. The addr bound check
             * is not a stand-in for that removed status check; it is a
             * separate, mandatory precondition for the g_lookup[addr]
             * access itself, since addr is loaded directly from this
             * record's (possibly corrupted or torn) header bytes.
             * Audit fix: this used to also require
             * hdr[0] == RECORD_STATUS_VALID, so a record whose status
             * byte alone got corrupted (e.g. a single bit flip - found
             * by property-based, randomized-fault stress testing) was
             * silently skipped here even though g_lookup still
             * correctly pointed at it, with the
             * same downstream consequence as the CRC case below: once
             * its source sector was erased, the address became
             * unrecoverable and a later eeprom_init() reported
             * EEPROM_NOT_FOUND for data that was, in fact, written.
             *
             * Migrating unconditionally (not gated on validate_record()
             * either) is the other half of this fix: GC's job is to
             * preserve the current record for its address, not to
             * eagerly judge its data integrity - that is deferred to
             * eeprom_read()'s lazy CRC check everywhere else in this
             * file (see validate_record()'s own doc comment on that
             * split). An earlier version only migrated a record here if
             * validate_record() reported EEPROM_OK, silently dropping
             * any record whose CRC no longer matched with the identical
             * NOT_FOUND-after-erase consequence. Migrating the bytes
             * as-is (corruption included) instead preserves the honest
             * EEPROM_CHECKSUM_ERROR a read would already have reported
             * before this GC ran.
             */
            uint32_t dst_free = g_config.sector_size - g_sectors[dst].write_offset;

            if (dst_free < (uint32_t)rec_total) {
                return EEPROM_SECTOR_FULL;
            }
            if (g_config.flash_read(rec_loc, buf, rec_total) != 0) {
                return EEPROM_WRITE_FAILED;
            }
            {
                uint32_t dst_loc = get_sector_address(dst) + g_sectors[dst].write_offset;

                if (g_config.flash_write(dst_loc, buf, rec_total) != 0) {
                    return EEPROM_WRITE_FAILED;
                }
                g_lookup[addr] = dst_loc;
                g_sectors[dst].write_offset += rec_total;
                g_stats.sector_usage[dst] = g_sectors[dst].write_offset;
            }
        }
        cur += rec_total;
    }
    return EEPROM_OK;
}

static eeprom_status_t perform_garbage_collection(void)
{
    int8_t src;
    int8_t dst;
    int8_t oldest;
    bool dst_is_spare;
    eeprom_status_t rc;

    src = find_gc_target_sector();
    if (src < 0) {
        return EEPROM_OK; /* nothing to collect */
    }
    if (g_active_sector < 0) {
        return EEPROM_FORMAT_ERROR;
    }

    /* Prefer an idle EMPTY sector as the compaction destination: it has
     * the sector's full capacity free, so src's survivors (which by
     * construction never exceeded one sector's worth of live data) are
     * always guaranteed to fit. Compacting into the current active sector
     * instead is only safe when it was *just* promoted (see ensure_room());
     * called at an arbitrary later time its remaining free space may be
     * too small, and it should not be relied upon when a genuine spare is
     * available.
     *
     * Audit fix: the spare is claimed via mark_gc_destination(), which
     * writes SECTOR_STATE_GC_DEST, NOT activate_sector()/ACTIVE. An
     * earlier version of this function used activate_sector() here, which
     * meant that for the entire window between this call and the
     * flash_erase(src) below, TWO sectors simultaneously read back as
     * ACTIVE if power was lost. eeprom_init()'s find_active_sector()
     * picks the first ACTIVE sector by physical index, with no way to
     * know which one was the real active sector - if the spare happened
     * to have a lower index than g_active_sector, init would erase the
     * wrong one, destroying live application data. GC_DEST closes that
     * gap: at most one sector can ever read back as ACTIVE, so recovery
     * is unambiguous regardless of index or sequence ordering. The header
     * is written here, before any record is copied into it, so even a
     * crash mid-compaction leaves it correctly marked (never mistaken for
     * EMPTY, so its partially-written tail is simply never treated as
     * "safe to write into without an erase" - see scan_sector_records()). */
    dst = find_empty_sector();
    dst_is_spare = (dst >= 0);
    if (!dst_is_spare) {
        dst = g_active_sector;
    } else {
        rc = mark_gc_destination((uint8_t)dst);
        if (rc != EEPROM_OK) {
            return rc;
        }
    }

    rc = compact_sector((uint8_t)src, (uint8_t)dst);
    if (rc != EEPROM_OK) {
        return rc;
    }

    if (g_config.flash_erase(get_sector_address((uint8_t)src), g_config.sector_size) != 0) {
        return EEPROM_ERASE_FAILED;
    }
    g_sectors[(uint8_t)src].erase_count += 1U;
    g_sectors[(uint8_t)src].sequence = 0;
    g_sectors[(uint8_t)src].state = SECTOR_STATE_EMPTY;
    g_sectors[(uint8_t)src].write_offset = SECTOR_HEADER_SIZE;
    persist_empty_sector_erase_count((uint8_t)src);
    g_stats.erase_count[(uint8_t)src] = g_sectors[(uint8_t)src].erase_count;
    g_stats.sector_usage[(uint8_t)src] = 0;
    g_stats.gc_runs += 1U;

    oldest = find_gc_target_sector();
    g_stats.oldest_sector = (oldest >= 0) ? (uint8_t)oldest : (uint8_t)g_active_sector;

    return EEPROM_OK;
}

static eeprom_status_t write_record(uint16_t addr, const uint8_t *data, uint8_t size)
{
    uint8_t *buf = g_write_record_buf;
    uint16_t rec_total = record_padded_size(size);
    uint16_t crc;
    uint32_t old_loc;
    uint32_t new_loc;
    uint16_t i;
    eeprom_status_t rc;

    rc = ensure_room(rec_total);
    if (rc != EEPROM_OK) {
        return rc;
    }

    for (i = 0; i < rec_total; i++) {
        buf[i] = 0xFFU;
    }
    buf[0] = RECORD_STATUS_VALID;
    store_u16(&buf[1], addr);
    buf[3] = size;
    (void)memcpy(&buf[RECORD_HEADER_SIZE], data, size);

    {
        uint8_t *crc_input = g_write_record_crc_input;

        crc_input[0] = buf[1];
        crc_input[1] = buf[2];
        crc_input[2] = buf[3];
        (void)memcpy(&crc_input[3], data, size);
        crc = crc16_ccitt(crc_input, (uint16_t)(3U + (uint16_t)size));
    }
    store_u16(&buf[(uint16_t)RECORD_HEADER_SIZE + size], crc);

    new_loc = get_sector_address((uint8_t)g_active_sector) + g_sectors[(uint8_t)g_active_sector].write_offset;
    if (g_config.flash_write(new_loc, buf, rec_total) != 0) {
        return EEPROM_WRITE_FAILED;
    }

    old_loc = g_lookup[addr];
    if (old_loc != LOOKUP_EMPTY) {
        invalidate_record(old_loc);
    }
    g_lookup[addr] = new_loc;
    g_sectors[(uint8_t)g_active_sector].write_offset += rec_total;
    g_stats.sector_usage[(uint8_t)g_active_sector] = g_sectors[(uint8_t)g_active_sector].write_offset;

    return EEPROM_OK;
}

/* --------------------------------------------------------------------- */
/* Initialization & scanning                                              */
/* --------------------------------------------------------------------- */

/**
 * Recovers from a partial write left behind by a power loss: invalidates
 * the broken trailing record (best effort) and permanently retires the
 * sector by transitioning it to FULL, since bytes past a broken record
 * cannot be trusted or safely reclaimed without a full erase.
 */
static void recover_partial_sector(uint8_t sector_index, uint32_t offset)
{
    invalidate_record(get_sector_address(sector_index) + offset);
    (void)write_sector_header(sector_index, g_sectors[sector_index].sequence,
                               SECTOR_STATE_FULL, g_sectors[sector_index].erase_count);
    g_sectors[sector_index].state = SECTOR_STATE_FULL;
}

/**
 * Walks every record in an ACTIVE or FULL sector, (re)populating the
 * lookup table with the newest record per address and recovering the
 * sector's true write offset. Must be called in ascending flash-log order
 * (sector activation order, then offset within a sector) so that "last
 * one scanned wins" reproduces "latest write wins".
 */
static void scan_sector_records(uint8_t sector_index)
{
    uint32_t base = get_sector_address(sector_index);
    uint32_t cur = SECTOR_HEADER_SIZE;
    bool broken_tail = false;

    while ((cur + RECORD_HEADER_SIZE) <= g_config.sector_size) {
        uint8_t hdr[RECORD_HEADER_SIZE];
        uint8_t attempt;
        int read_rc = -1;

        for (attempt = 0; attempt < SCAN_HEADER_READ_RETRIES; attempt++) {
            read_rc = g_config.flash_read(base + cur, hdr, RECORD_HEADER_SIZE);
            if (read_rc == 0) {
                break;
            }
        }
        if (read_rc != 0) {
            /* Every attempt faulted - audit fix: an earlier version gave
             * up after one flash_read() failure here and treated it
             * exactly like a genuinely truncated sector tail, which (for
             * an ACTIVE sector) drove recover_partial_sector() to
             * PERMANENTLY invalidate whatever record was actually at
             * this offset via a real Flash write - destroying data that,
             * per this file's own READ_FAILED semantics, may never have
             * been corrupted at all, only transiently unreadable on that
             * one attempt (found by property-based stress testing that
             * injected standalone random read failures with no data
             * corruption whatsoever and still lost previously-committed
             * records). Still treated as
             * broken_tail after exhausting retries: at that point a
             * genuinely undecodable header is the most defensible
             * remaining assumption. */
            broken_tail = true;
            break;
        }
        if ((hdr[0] == 0xFFU) && (hdr[1] == 0xFFU) && (hdr[2] == 0xFFU) && (hdr[3] == 0xFFU)) {
            break; /* first free slot: end of used region */
        }

        {
            uint8_t status = hdr[0];
            uint16_t addr = load_u16(&hdr[1]);
            uint8_t size = hdr[3];
            bool addr_ok = (addr <= EEPROM_MAX_VIRTUAL_ADDR);
            bool size_ok = (size >= 1U);
            uint16_t rec_total = 0;
            bool fits = false;

            if (addr_ok && size_ok) {
                rec_total = record_padded_size(size);
                fits = ((cur + (uint32_t)rec_total) <= g_config.sector_size);
            }

            if ((!addr_ok) || (!size_ok) || (!fits)) {
                /* Length itself is undecodable (or would overrun the
                 * sector) - this, and only this, is the case where we
                 * genuinely cannot know how many bytes to skip to find
                 * the next record, so it's the one legitimate reason to
                 * stop scanning here (see recover_partial_sector() for
                 * ACTIVE sectors below). */
                broken_tail = true;
                break;
            }

            /* A structurally decodable header (addr/size in range, fits
             * the sector) whose status byte is neither VALID nor INVALID
             * means THIS record's own bytes were corrupted after being
             * fully committed - e.g. a single bit flip applied post-write
             * (found by property-based stress testing) - not that the
             * sector's tail was cut off by a power loss. Its length is
             * still trustworthy,
             * so unlike the undecodable case above, it's both safe AND
             * necessary to skip over it (like an explicitly
             * RECORD_STATUS_INVALID record already is) and keep
             * scanning, rather than stopping.
             *
             * Audit fix: this used to be folded into the same
             * broken_tail path as an undecodable header, which meant any
             * single corrupted record silently dropped every record
             * physically written after it in the same sector from
             * g_lookup - including perfectly intact, later ones - the
             * moment this sector was rescanned (e.g. on the next
             * eeprom_init()). Property-based stress testing (randomized
             * bit-corruption + simulated reboots) surfaced this as
             * EEPROM_NOT_FOUND for addresses with a confirmed prior
             * write.
             */
            if (status == RECORD_STATUS_VALID) {
                g_lookup[addr] = base + cur;
            }
            cur += rec_total;
        }
    }

    g_sectors[sector_index].write_offset = cur;
    if (broken_tail && (g_sectors[sector_index].state == SECTOR_STATE_ACTIVE)) {
        recover_partial_sector(sector_index, cur);
    }
}

/**
 * Reads and classifies every sector's header. A header is treated as
 * corrupt if its state byte is not one of EMPTY/ACTIVE/FULL, or if it
 * claims a non-EMPTY state while its sequence number still reads as the
 * erased marker (an inconsistent combination that cannot arise from
 * normal operation). Corrupt sectors are erased immediately and marked
 * EMPTY, per the spec's documented recovery strategy.
 */
static void scan_all_sectors(void)
{
    uint8_t i;

    for (i = 0; i < g_config.num_sectors; i++) {
        uint32_t seq = 0;
        uint8_t state = SECTOR_STATE_EMPTY;
        uint32_t erase_cnt = 0;
        bool corrupt;

        if (read_sector_header(i, &seq, &state, &erase_cnt) != 0) {
            corrupt = true;
        } else if ((state != SECTOR_STATE_EMPTY) && (state != SECTOR_STATE_ACTIVE) &&
                   (state != SECTOR_STATE_FULL) && (state != SECTOR_STATE_GC_DEST)) {
            corrupt = true;
        } else if ((state != SECTOR_STATE_EMPTY) && (seq == SEQUENCE_ERASED_MARK)) {
            corrupt = true;
        } else {
            corrupt = false;
        }

        if (corrupt) {
            (void)g_config.flash_erase(get_sector_address(i), g_config.sector_size);
            g_sectors[i].sequence = 0;
            g_sectors[i].state = SECTOR_STATE_EMPTY;
            g_sectors[i].erase_count = 0;
            g_sectors[i].write_offset = SECTOR_HEADER_SIZE;
            persist_empty_sector_erase_count(i);
        } else {
            g_sectors[i].sequence = seq;
            g_sectors[i].state = state;
            /* A sector whose header has never been written (blank/erased
             * silicon, or freshly erased by us) reads erase_count back as
             * the erased marker, not a real count - treat that as 0 rather
             * than a huge bogus lifetime count. */
            g_sectors[i].erase_count = (erase_cnt == SEQUENCE_ERASED_MARK) ? 0U : erase_cnt;
            g_sectors[i].write_offset = SECTOR_HEADER_SIZE;
        }
        g_stats.erase_count[i] = g_sectors[i].erase_count;
    }
}

/**
 * Runs scan_sector_records() over every ACTIVE/FULL/GC_DEST sector in
 * ascending sequence order (i.e. true activation/chronological order),
 * NOT physical sector index order. This matters: scan_sector_records()
 * relies on "last one scanned for a given address wins" to reconstruct
 * "latest write wins" in g_lookup, which is only correct if the scan
 * order matches the order records were actually written in. Physical
 * index order coincides with activation order only up until the first
 * time a low-indexed sector is reused after a later-indexed one is
 * already active - which happens routinely once wear-leveling rotation
 * wraps around. In the normal case this is masked by invalidate_record()
 * correctly clearing stale records' status bytes, but a record whose
 * invalidation write itself failed (a real Flash fault) followed by a
 * reboot before the next GC cleans it up would, under index-order
 * scanning, have a chance of incorrectly winning over a genuinely newer
 * record living in a lower-indexed sector. Sequence order removes that
 * dependency on invalidate_record() succeeding. O(n^2) in num_sectors
 * (<=8), negligible.
 */
static void scan_records_in_activation_order(void)
{
    bool scanned[EEPROM_MAX_SECTORS];
    uint8_t n;
    uint8_t i;

    for (i = 0; i < g_config.num_sectors; i++) {
        scanned[i] = false;
    }

    for (n = 0; n < g_config.num_sectors; n++) {
        int8_t pick = -1;

        for (i = 0; i < g_config.num_sectors; i++) {
            bool eligible = (!scanned[i]) &&
                             ((g_sectors[i].state == SECTOR_STATE_ACTIVE) ||
                              (g_sectors[i].state == SECTOR_STATE_FULL) ||
                              (g_sectors[i].state == SECTOR_STATE_GC_DEST));

            if (eligible && ((pick < 0) || (g_sectors[i].sequence < g_sectors[(uint8_t)pick].sequence))) {
                pick = (int8_t)i;
            }
        }
        if (pick < 0) {
            break; /* nothing left to scan */
        }
        scanned[(uint8_t)pick] = true;
        scan_sector_records((uint8_t)pick);
        g_stats.sector_usage[(uint8_t)pick] = g_sectors[(uint8_t)pick].write_offset;
    }
}

/* --------------------------------------------------------------------- */
/* Public API                                                             */
/* --------------------------------------------------------------------- */

eeprom_status_t eeprom_init(const eeprom_config_t *config)
{
    uint8_t i;
    uint32_t addr_idx;
    uint32_t max_seq = 0;
    bool have_seq = false;
    int8_t chosen_active = -1;
    eeprom_status_t rc;

    if ((config == NULL) || (config->flash_read == NULL) || (config->flash_write == NULL) ||
        (config->flash_erase == NULL) || (config->sector_size == 0U) ||
        (config->num_sectors < 2U) || (config->num_sectors > EEPROM_MAX_SECTORS) ||
        ((config->write_width != 4U) && (config->write_width != 8U))) {
        return EEPROM_FORMAT_ERROR;
    }

    g_config = *config;
    g_initialized = false;
    g_active_sector = -1;
    g_next_sequence = 0;

    g_stats.total_writes = 0;
    g_stats.total_reads = 0;
    g_stats.gc_runs = 0;
    g_stats.oldest_sector = 0;
    for (i = 0; i < EEPROM_MAX_SECTORS; i++) {
        g_stats.erase_count[i] = 0;
        g_stats.sector_usage[i] = 0;
    }
    for (addr_idx = 0; addr_idx < EEPROM_VIRTUAL_SIZE; addr_idx++) {
        g_lookup[addr_idx] = LOOKUP_EMPTY;
    }

    scan_all_sectors();

    /* At most one ACTIVE sector is legal; find_active_sector() picks the
     * first one (lowest index) as authoritative if duplicates exist. */
    chosen_active = find_active_sector();

    for (i = 0; i < g_config.num_sectors; i++) {
        if (g_sectors[i].state != SECTOR_STATE_EMPTY) {
            if ((!have_seq) || (g_sectors[i].sequence > max_seq)) {
                max_seq = g_sectors[i].sequence;
                have_seq = true;
            }
        }
    }
    g_next_sequence = have_seq ? (max_seq + 1U) : 0U;

    /* Force any extra ACTIVE sectors (an anomalous/corrupted condition)
     * back to EMPTY - only the one find_active_sector() picked stays. */
    for (i = 0; i < g_config.num_sectors; i++) {
        if ((g_sectors[i].state == SECTOR_STATE_ACTIVE) && ((int8_t)i != chosen_active)) {
            (void)g_config.flash_erase(get_sector_address(i), g_config.sector_size);
            g_sectors[i].erase_count += 1U;
            g_sectors[i].sequence = 0;
            g_sectors[i].state = SECTOR_STATE_EMPTY;
            g_sectors[i].write_offset = SECTOR_HEADER_SIZE;
            persist_empty_sector_erase_count(i);
            g_stats.erase_count[i] = g_sectors[i].erase_count;
        }
    }

    if (chosen_active < 0) {
        int8_t empty_idx = find_empty_sector();

        if (empty_idx < 0) {
            return EEPROM_FORMAT_ERROR;
        }
        rc = activate_sector((uint8_t)empty_idx);
        if (rc != EEPROM_OK) {
            return rc;
        }
        chosen_active = empty_idx;
    }
    g_active_sector = chosen_active;

    scan_records_in_activation_order();

    /* Recovery above may have retired the active sector to FULL; make sure
     * a writable ACTIVE sector exists before returning to the caller. */
    if (g_sectors[(uint8_t)g_active_sector].state != SECTOR_STATE_ACTIVE) {
        int8_t empty_idx = find_empty_sector();

        if (empty_idx < 0) {
            return EEPROM_FORMAT_ERROR;
        }
        rc = activate_sector((uint8_t)empty_idx);
        if (rc != EEPROM_OK) {
            return rc;
        }
        g_active_sector = empty_idx;
    }

    {
        int8_t oldest = find_gc_target_sector();
        g_stats.oldest_sector = (oldest >= 0) ? (uint8_t)oldest : (uint8_t)g_active_sector;
    }

    g_initialized = true;
    return EEPROM_OK;
}

eeprom_status_t eeprom_write(uint16_t addr, const uint8_t *data, uint8_t size)
{
    eeprom_status_t rc;

    if (!g_initialized) {
        return EEPROM_UNINITIALIZED;
    }
    if (addr > EEPROM_MAX_VIRTUAL_ADDR) {
        return EEPROM_INVALID_ADDR;
    }
    if ((data == NULL) || (size == 0U)) {
        return EEPROM_INVALID_SIZE;
    }

    rc = write_record(addr, data, size);
    if (rc == EEPROM_OK) {
        g_stats.total_writes += 1U;
    }
    return rc;
}

eeprom_status_t eeprom_read(uint16_t addr, uint8_t *data, uint8_t *size)
{
    uint32_t flash_addr;
    uint16_t data_size;
    eeprom_status_t rc;

    if (!g_initialized) {
        return EEPROM_UNINITIALIZED;
    }
    if (addr > EEPROM_MAX_VIRTUAL_ADDR) {
        return EEPROM_INVALID_ADDR;
    }
    if ((data == NULL) || (size == NULL)) {
        return EEPROM_INVALID_SIZE;
    }
    if (find_record(addr, &flash_addr) != 0) {
        return EEPROM_NOT_FOUND;
    }
    rc = validate_record(flash_addr, &data_size);
    if (rc != EEPROM_OK) {
        /* EEPROM_CHECKSUM_ERROR (data itself is bad) or EEPROM_READ_FAILED
         * (the flash_read() callback faulted; the data may be fine) - see
         * validate_record()'s doc comment. Audit fix, Claim 4: these used
         * to both collapse to EEPROM_CHECKSUM_ERROR here, which misled
         * callers into treating a transient I/O fault as permanent data
         * corruption. */
        return rc;
    }
    if (data_size > (uint16_t)(*size)) {
        /* Audit fix, Claim 5: report the size actually needed so the
         * caller can size a retry buffer, instead of leaving *size
         * unchanged with no way to know how big a buffer to allocate. */
        *size = (uint8_t)data_size;
        return EEPROM_INVALID_SIZE;
    }
    if (g_config.flash_read(flash_addr + RECORD_HEADER_SIZE, data, data_size) != 0) {
        return EEPROM_READ_FAILED;
    }

    *size = (uint8_t)data_size;
    g_stats.total_reads += 1U;
    return EEPROM_OK;
}

bool eeprom_exists(uint16_t addr)
{
    if ((!g_initialized) || (addr > EEPROM_MAX_VIRTUAL_ADDR)) {
        return false;
    }
    return (g_lookup[addr] != LOOKUP_EMPTY);
}

eeprom_status_t eeprom_get_stats(eeprom_stats_t *stats)
{
    uint8_t i;

    if (!g_initialized) {
        return EEPROM_UNINITIALIZED;
    }
    if (stats == NULL) {
        return EEPROM_FORMAT_ERROR;
    }
    *stats = g_stats;
    /* Sector state lives in g_sectors, not g_stats: unlike erase_count/
     * sector_usage (mirrored into g_stats at every transition site so
     * they read consistently even mid-operation), state is read straight
     * from the live source of truth here rather than adding a fifth
     * mirroring call site for a diagnostics-only field. */
    for (i = 0; i < g_config.num_sectors; i++) {
        stats->sector_state[i] = g_sectors[i].state;
    }
    return EEPROM_OK;
}

eeprom_status_t eeprom_garbage_collect(void)
{
    if (!g_initialized) {
        return EEPROM_UNINITIALIZED;
    }
    return perform_garbage_collection();
}

eeprom_status_t eeprom_format(void)
{
    uint8_t i;
    uint32_t addr_idx;
    int8_t empty_idx;
    eeprom_status_t rc;

    if (!g_initialized) {
        return EEPROM_UNINITIALIZED;
    }

    for (i = 0; i < g_config.num_sectors; i++) {
        if (g_config.flash_erase(get_sector_address(i), g_config.sector_size) != 0) {
            return EEPROM_ERASE_FAILED;
        }
        g_sectors[i].erase_count += 1U;
        g_sectors[i].sequence = 0;
        g_sectors[i].state = SECTOR_STATE_EMPTY;
        g_sectors[i].write_offset = SECTOR_HEADER_SIZE;
        persist_empty_sector_erase_count(i);
        g_stats.erase_count[i] = g_sectors[i].erase_count;
        g_stats.sector_usage[i] = 0;
    }
    for (addr_idx = 0; addr_idx < EEPROM_VIRTUAL_SIZE; addr_idx++) {
        g_lookup[addr_idx] = LOOKUP_EMPTY;
    }
    g_next_sequence = 0;

    empty_idx = find_empty_sector();
    if (empty_idx < 0) {
        return EEPROM_FORMAT_ERROR;
    }
    rc = activate_sector((uint8_t)empty_idx);
    if (rc != EEPROM_OK) {
        return rc;
    }
    g_active_sector = empty_idx;
    g_stats.oldest_sector = (uint8_t)g_active_sector;

    return EEPROM_OK;
}
