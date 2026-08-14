# EEPROM Emulation Library Test Cases

## Test Environment Setup

```c
// Mock flash driver for testing
uint8_t mock_flash[3 * 64 * 1024];  // 3 x 64KB sectors

int flash_read_mock(uint32_t addr, uint8_t *data, uint16_t size) {
    memcpy(data, &mock_flash[addr - FLASH_BASE], size);
    return 0;
}

int flash_write_mock(uint32_t addr, const uint8_t *data, uint16_t size) {
    uint32_t offset = addr - FLASH_BASE;
    // Simulate write (can only clear bits, not set them)
    for (int i = 0; i < size; i++) {
        mock_flash[offset + i] &= data[i];
    }
    return 0;
}

int flash_erase_mock(uint32_t addr, uint32_t size) {
    uint32_t offset = addr - FLASH_BASE;
    memset(&mock_flash[offset], 0xFF, size);  // Erase = 0xFF
    return 0;
}

eeprom_config_t test_config = {
    .flash_start = FLASH_BASE,
    .sector_size = 64 * 1024,
    .num_sectors = 3,
    .write_width = 4,
    .flash_read = flash_read_mock,
    .flash_write = flash_write_mock,
    .flash_erase = flash_erase_mock,
};
```

---

## Test Cases

### TC-001: Basic Write and Read

**Purpose**: Verify fundamental write/read cycle works correctly.

**Setup**: Initialize with empty flash.

**Steps**:
1. Write 4 bytes to address 0x0001: `{0x11, 0x22, 0x33, 0x44}`
2. Read back from address 0x0001
3. Verify data matches

**Expected Result**: Read returns exactly `{0x11, 0x22, 0x33, 0x44}`, EEPROM_OK

**Validation**: Data integrity verified, no corruption

---

### TC-002: Overwrite Same Address

**Purpose**: Verify that overwriting updates value and invalidates old record.

**Setup**: TC-001 passed.

**Steps**:
1. Write new data to address 0x0001: `{0xAA, 0xBB, 0xCC, 0xDD}`
2. Read from address 0x0001
3. Verify old data is replaced

**Expected Result**: Read returns `{0xAA, 0xBB, 0xCC, 0xDD}`, EEPROM_OK

**Validation**: Only latest write visible; old record marked invalid

---

### TC-003: Multiple Addresses

**Purpose**: Verify multiple distinct addresses can be stored simultaneously.

**Setup**: Empty flash.

**Steps**:
1. Write 2 bytes to address 0x0001: `{0x11, 0x22}`
2. Write 3 bytes to address 0x0002: `{0x33, 0x44, 0x55}`
3. Write 1 byte to address 0x0003: `{0x66}`
4. Read all three addresses
5. Verify each returns correct data independently

**Expected Result**: All reads succeed with correct data, no cross-contamination

**Validation**: Virtual address isolation works correctly

---

### TC-004: Boundary Values

**Purpose**: Verify edge cases (max address, max data size).

**Setup**: Empty flash.

**Steps**:
1. Write 255 bytes to address 0x0FFF (max address): `{0xFF, 0xFE, ..., 0x00}`
2. Read from address 0x0FFF
3. Verify all 255 bytes correct

**Expected Result**: Write succeeds, read returns all 255 bytes correctly

**Validation**: Full-size data handled correctly

---

### TC-005: Invalid Address Rejection

**Purpose**: Verify out-of-range addresses are rejected.

**Setup**: Initialized library.

**Steps**:
1. Try to write to address 0x1000 (out of range)
2. Try to read from address 0xFFFF
3. Try to write to address 0x10000

**Expected Result**: All operations return EEPROM_INVALID_ADDR

**Validation**: Boundary checking prevents silent corruption

---

### TC-006: Invalid Size Rejection

**Purpose**: Verify oversized writes are rejected.

**Setup**: Initialized library.

**Steps**:
1. Try to write 256 bytes (exceeds max 255)
2. Try to write 1000 bytes
3. Try to write 0 bytes

**Expected Result**: All return EEPROM_INVALID_SIZE

**Validation**: Size validation prevents data corruption

---

### TC-007: Not Found Case

**Purpose**: Verify reads to unwritten addresses return EEPROM_NOT_FOUND.

**Setup**: Empty flash.

**Steps**:
1. Read from address 0x0100 (never written)
2. Verify error code
3. Verify buffer unchanged

**Expected Result**: Returns EEPROM_NOT_FOUND, buffer not modified

**Validation**: Missing data distinguished from corruption

---

### TC-008: Sector Wear-Leveling Trigger

**Purpose**: Verify sector switches when full and wear-leveling distributes writes.

**Setup**: 3 sectors, empty flash.

**Steps**:
1. Calculate data size to fill sector 0 (write until sector state = FULL)
2. Write final record to trigger sector 1 activation
3. Verify sector 1 is now active
4. Check erase counts are equal
5. Repeat until all sectors used
6. Verify round-robin sequence

**Expected Result**: Sectors activate in order (0→1→2→0...), erase counts balanced

**Validation**: Round-robin wear-leveling works

---

### TC-009: Garbage Collection Basic

**Purpose**: Verify garbage collection compacts sectors correctly.

**Setup**: 2 sectors filled with data.

**Steps**:
1. Fill sector 0 with records (mixed addresses)
2. Move to sector 1, fill with records
3. Invalidate 50% of records in sector 0 (by overwriting)
4. Force garbage collection
5. Verify all valid records still readable
6. Verify sector 0 erased and reused

**Expected Result**: Data compacted, invalid records dropped, all valid data preserved

**Validation**: GC does not lose data; sector reused correctly

---

### TC-010: Corrupted Sector Recovery

**Purpose**: Verify library detects and recovers from corrupted sector header.

**Setup**: Sector with valid data and corrupted header (invalid sequence number).

**Steps**:
1. Manually corrupt sector 1 header (set sequence = 0xFFFFFFFF)
2. Initialize library
3. Verify library detects corruption
4. Verify sector marked for recovery
5. Write new data and trigger GC
6. Verify corrupted sector erased and reused

**Expected Result**: Corruption detected, recovery succeeds, no data loss from uncorrupted sectors

**Validation**: Robustness against power-loss scenarios

---

### TC-011: CRC Checksum Verification

**Purpose**: Verify CRC detects data corruption and prevents silent corruption.

**Setup**: Sector with valid data.

**Steps**:
1. Write known data to address 0x0001
2. Manually flip a bit in stored data (corrupt it)
3. Corrupt matching CRC byte also (try to hide corruption)
4. Read from address 0x0001
5. Verify CRC mismatch detected

**Expected Result**: Returns EEPROM_CHECKSUM_ERROR, data not returned

**Validation**: CRC protection prevents silent corruption

---

### TC-012: Stress Test - Many Overwrites

**Purpose**: Verify wear-leveling under stress (many writes to same address).

**Setup**: Empty flash.

**Steps**:
1. Write to address 0x0001 1000 times with different data
2. Each write followed by read to verify
3. Check wear is balanced across sectors
4. Verify final data correct

**Expected Result**: All 1000 writes succeed, final data correct, GC runs multiple times

**Validation**: Sustained write load handled correctly

---

### TC-013: Stress Test - Random Addresses

**Purpose**: Verify random access patterns work correctly.

**Setup**: Empty flash.

**Steps**:
1. Generate 500 random write operations (random addr, random data)
2. Log all operations
3. Verify each read matches corresponding write
4. Cross-verify: Random reads of random addresses match logged writes

**Expected Result**: All 500 writes and reads succeed, no mismatches

**Validation**: Random access patterns work correctly

---

### TC-014: Power Loss Simulation

**Purpose**: Verify recovery after simulated power loss during write.

**Setup**: Sector with valid data.

**Steps**:
1. Fill sector 0 completely
2. Manually truncate write in sector 1 (simulate power loss mid-write)
3. Set sector 1 state to PARTIAL
4. Initialize library again
5. Attempt to write/read
6. Verify recovery works, library continues normally

**Expected Result**: Recovery succeeds, data before power loss intact, partial write discarded

**Validation**: Crash recovery works

---

### TC-015: Initialization of Pre-Populated Flash

**Purpose**: Verify library correctly reads existing data on initialization.

**Setup**: Pre-populate flash with known data, simulate device power cycle.

**Steps**:
1. Write 10 records to various addresses
2. Call eeprom_init() again (simulating power cycle)
3. Read all 10 addresses
4. Verify all data matches what was written before init

**Expected Result**: All pre-existing data readable after re-init, no data loss

**Validation**: Persistence across power cycles verified

---

### TC-016: Statistics Tracking

**Purpose**: Verify library tracks statistics accurately.

**Setup**: Execute multiple operations.

**Steps**:
1. Perform 50 write operations
2. Perform 100 read operations
3. Force 3 garbage collection runs
4. Call eeprom_get_stats()
5. Verify counts match expected

**Expected Result**: 
- total_writes >= 50
- total_reads >= 100
- gc_runs >= 3
- erase counts and sector usage reasonable

**Validation**: Statistics accurate for diagnostics

---

### TC-017: Format (Full Erase)

**Purpose**: Verify factory reset clears all data.

**Setup**: Sectors filled with data.

**Steps**:
1. Fill all sectors with data
2. Call eeprom_format()
3. Try to read any address
4. Try to write new data
5. Verify both succeed cleanly

**Expected Result**: Format succeeds, all old data gone, new writes work

**Validation**: Factory reset works correctly

---

### TC-018: Sector State Transitions

**Purpose**: Verify all sector state transitions (EMPTY → ACTIVE → FULL → GC) work correctly.

**Setup**: 3 sectors, empty flash.

**Steps**:
1. Track each sector's state through full write cycle
2. Verify EMPTY → ACTIVE transition
3. Verify ACTIVE → FULL transition
4. Verify FULL sector GC → EMPTY
5. Log state at each step

**Expected Result**: All state transitions occur in correct order

**Validation**: State machine logic correct

---

### TC-019: Lookup Table Integrity

**Purpose**: Verify in-memory lookup table stays consistent with flash.

**Setup**: Multiple writes to same address.

**Steps**:
1. Write to address 0x0050 (data A)
2. Write to address 0x0050 (data B)
3. Read address 0x0050 (should be B)
4. Write to address 0x0051
5. Read address 0x0050 again (should still be B, not A)
6. Verify lookup table has new record pointed to, old invalidated

**Expected Result**: Latest write always returned, lookup accurate

**Validation**: Lookup table correctness

---

### TC-020: MISRA-C Compliance Check

**Purpose**: Verify generated code passes static analysis.

**Setup**: Compiled with -Wall -Wextra flags.

**Steps**:
1. Compile eeprom.c with clang or gcc at -Wall -Wextra
2. Run MISRA checker (e.g., Cppcheck, PRQA)
3. Document any deviations
4. Verify no high-severity violations

**Expected Result**: Compilation clean, static analysis passes, deviations documented

**Validation**: Production code quality

---

## Summary

**Total Test Cases**: 20

**Categories**:
- Functionality: TC-001 to TC-007 (basic operations)
- Wear-Leveling: TC-008, TC-009 (sector management)
- Reliability: TC-010, TC-011, TC-014 (recovery and corruption)
- Stress: TC-012, TC-013 (sustained load)
- Persistence: TC-015, TC-016, TC-017 (data retention)
- Verification: TC-018, TC-019, TC-020 (internal consistency)

**Passing Criteria**: All 20 tests pass with expected results.

---

**End of Test Cases**
