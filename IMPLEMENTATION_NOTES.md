# Implementation Notes

Companion to [inc/eeprom.h](inc/eeprom.h) and [src/eeprom.c](src/eeprom.c),
implementing [EEPROM_SPEC.md](EEPROM_SPEC.md) and verified against
[EEPROM_TEST_CASES.md](EEPROM_TEST_CASES.md) via [test/test_eeprom.c](test/test_eeprom.c).

## Independent audit findings and fixes applied

This library went through one round of independent code review after the
initial implementation (20/20 tests passing, clean compile, clean ASan/UBSan)
was already delivered. The review found a **Critical, silent-data-loss bug**
that the original test suite did not catch, plus several smaller issues. All
of the following were confirmed by tracing the actual code (not just the
review's description of it) and are now fixed and regression-tested:

- **Critical — two simultaneously-ACTIVE sectors during manual GC.**
  `eeprom_garbage_collect()`, when called manually while an idle spare
  sector existed, promoted that spare to `SECTOR_STATE_ACTIVE` (via
  `activate_sector()`) as its compaction destination, while
  `g_active_sector` still pointed at the real, live active sector. For the
  entire window between that promotion and the reclaimed source sector's
  erase, two sectors read back as `ACTIVE`. If power was lost in that
  window, `eeprom_init()`'s `find_active_sector()` (first match by
  physical index) had roughly even odds of picking the transient GC
  destination instead of the real active sector — and then the "force
  extra ACTIVE sectors back to EMPTY" recovery loop **erased the real
  active sector outright**, destroying live application data with no
  possibility of recovery. Fixed by giving GC destinations a distinct,
  unambiguous state, `SECTOR_STATE_GC_DEST` (0xFD — reachable from `EMPTY`
  by clearing a different bit than the `ACTIVE`/`FULL` chain uses), written
  via a new `mark_gc_destination()` instead of `activate_sector()`. At most
  one sector can now ever read back as `ACTIVE`, so this ambiguity cannot
  exist regardless of physical index or sequence number. `find_gc_target_sector()`
  now treats `FULL` and `GC_DEST` as equally reclaimable; `scan_all_sectors()`
  and the init-time record scan both recognize `GC_DEST` as a legal,
  non-corrupt state. See `perform_garbage_collection()`'s and
  `mark_gc_destination()`'s doc comments for the full reasoning.

  Worth noting explicitly: a proposed alternative fix — having
  `find_active_sector()` prefer the *highest sequence number* instead of
  first-index-match — would have made this bug **strictly worse**, not
  better: the GC destination is always assigned a higher sequence than the
  real active sector (it's promoted later), so that heuristic would pick
  the wrong sector 100% of the time instead of only when index order
  happened to be unlucky. Neither index nor sequence can distinguish "the
  real active sector" from "a sector mid-GC" once both are encoded as the
  same state value — only a distinct state can.

- **Adjacent bug found while fixing the above — sectors scanned in
  physical index order, not activation order.** `eeprom_init()`'s
  record-rebuild loop scanned sectors 0..N-1 by index. `scan_sector_records()`
  relies on "last one scanned for an address wins" to reconstruct "latest
  write wins," which is only correct if scan order matches chronological
  (sequence) order — true only until wear-leveling rotation wraps around
  and a low-indexed sector ends up holding *newer* data than a
  higher-indexed one. Masked in the common case because `invalidate_record()`
  usually correctly clears stale records regardless of scan order; only
  exposed if an invalidation write itself failed (a real Flash fault) and
  a reboot followed before the next GC cleaned it up. Fixed with a new
  `scan_records_in_activation_order()` that sorts by sequence number
  before scanning (O(n²) in `num_sectors <= 8`, negligible cost).

- **Major — `eeprom_read()` returned `EEPROM_CHECKSUM_ERROR` for a
  physical Flash read failure**, indistinguishable from genuine data
  corruption, even though the underlying data might be perfectly intact
  and a retry might succeed. Added `EEPROM_READ_FAILED` to `eeprom_status_t`
  (value 10 — purely additive, does not renumber 0-9) and had
  `validate_record()` itself return the correct one of `EEPROM_OK` /
  `EEPROM_CHECKSUM_ERROR` / `EEPROM_READ_FAILED` rather than a bare
  success/failure `int`, so the distinction survives all the way to the
  caller.

- **Minor — `eeprom_read()` didn't report the required buffer size on
  `EEPROM_INVALID_SIZE`.** `*size` is now set to the record's actual size
  before returning in that case, so a caller can size a retry buffer
  instead of guessing.

- **Major — worst-case stack depth on the write/GC call chain.**
  `write_record()`'s and `compact_sector()`'s ~260-byte working buffers
  moved from the stack to `static` storage, removing roughly 800 bytes of
  worst-case depth from `eeprom_write() -> ... -> compact_sector()`, a
  real overflow risk against a 1 KiB Cortex-M task stack. Deliberately
  **not** applied to `validate_record()`, even though its buffers are
  comparable in size: `validate_record()` is also called from
  `eeprom_read()`, which the header documents as ISR-safe/reentrant with
  an in-progress write — giving it a static buffer would let an
  ISR-context read race a main-thread write's GC pass over the same
  memory, trading a stack-overflow risk for a data-race bug. See the
  comment above `g_write_record_buf` in eeprom.c.

- **Major — fixed 4096-entry, 16 KiB `g_lookup` table with no way to
  shrink it.** `EEPROM_MAX_VIRTUAL_ADDR` in eeprom.h is now overridable at
  compile time (`#ifndef` guard), so an integrator whose application only
  needs, say, 64 addresses can define it down to `0x003FU` before
  including the header and shrink the lookup table from 16 KiB to 256
  bytes. A compile-time `#if` check in eeprom.c enforces the one
  correctness constraint this interacts with: the value must stay below
  `0xFF00`, or the free-space-detection trick (an all-`0xFF` record header
  unambiguously means "never written") would no longer hold.

- **Regression test added**: `AUDIT-1` in test_eeprom.c specifically
  engineers the failure window above (a spare sector at a *lower* physical
  index than the real active sector, matching the arrangement that
  actually triggers the original bug) and injects a targeted
  `flash_erase()` failure to simulate power loss between the GC
  destination being committed and the source sector's erase. **Verified
  this test fails against the pre-fix code** (reverted the fix locally,
  rebuilt, confirmed `AUDIT-1` fails with "guard data lost across
  simulated crash," then restored the fix and confirmed it passes again)
  — this wasn't just written to pass, it was checked to actually
  discriminate between the buggy and fixed behavior.

- **Not changed, reviewed and judged correct as-is**: the CRC16/AND-only
  Flash-write encoding (architecturally sound, confirmed by direct code
  reading); TC-008's wear-leveling balance evidence (reasonable, though
  only one configuration/workload — additional configs would strengthen
  it further, not fix a defect); the general absence of hidden UB/strict-
  aliasing issues (re-confirmed clean under ASan/UBSan after all fixes
  above).

- **Flagged, not fixed here (needs target-hardware verification, not a
  code change)**: several STM32 Flash families with ECC-protected Flash
  (many L4/G4/H7/U5 parts) do not reliably support a second partial
  program to an already-programmed word, even one that only clears
  additional bits — which is exactly the pattern this library's
  invalidate/state-transition writes rely on. F1/F4 (non-ECC) are
  generally fine; this must be checked against the actual target part's
  Flash/ECC behavior before shipping on any newer STM32 family. The host
  mock's simple AND-semantics model cannot catch this, since it doesn't
  model ECC at all.

## Property-based stress testing findings (2026-08-16)

A property-based test harness drove this library's actual compiled
`src/eeprom.c` — not a reimplementation — through a `ctypes` bridge,
under a randomized-operation, randomized-fault property-based scheme:
seeded writes/reads/forced-GC/simulated-reboots mixed with seeded
write/erase/read failures, partial (torn) writes, and post-commit bit
corruption, checked against a reference model after every operation.
This surfaced four real bugs the existing 21-test suite didn't (three of
which are now fixed here; the fourth remains open — see below), plus
confirmed several findings that are real but out of scope for a code fix.

**Fixed:**

- **A sector's lifetime `erase_count` was lost across a reboot that
  caught it sitting `EMPTY`** (erased by GC, but not yet reactivated).
  `erase_count` was only ever persisted to flash inside
  `activate_sector()`/`mark_gc_destination()`; a sector's header while
  merely `EMPTY` is fully blank, so `read_sector_header()` saw the
  erased-marker value there and `scan_all_sectors()` — correctly, for a
  header that's genuinely never been written — reported 0. Fixed with a
  new `persist_empty_sector_erase_count()`, called immediately after
  every erase-to-`EMPTY` transition (GC reclaim, `eeprom_init()`'s
  extra-`ACTIVE` recovery, `eeprom_format()`, and the corrupt-header
  recovery path). Deliberately writes `SEQUENCE_ERASED_MARK` (not `0`)
  for the sequence field in that call: under this format's AND-only
  write semantics, persisting a real `0` there would permanently trap
  the field (a later Flash write can only clear bits, never set them),
  so a future `activate_sector()` could never AND-narrow it back up to a
  real sequence number. Writing back the value it already reads as
  (`0xFFFFFFFF`) is a true no-op for that field, leaving it free for a
  real value later while still durably recording `erase_count`.

- **`compact_sector()` silently dropped (did not migrate) a live
  record during GC whenever its status byte no longer read
  `RECORD_STATUS_VALID` or its CRC no longer validated** — e.g. after a
  single bit flip applied post-commit. `g_lookup[addr]` was left
  pointing at the record's old location; once the source sector was
  erased moments later, that address became permanently unrecoverable,
  and the next `eeprom_init()` (rebuilding `g_lookup` purely by
  rescanning flash) reported `EEPROM_NOT_FOUND` — silently
  misrepresenting "this data is corrupted" as "this address was never
  written." Fixed by migrating a record unconditionally whenever
  `g_lookup[addr] == rec_loc` (this file's own documented source of
  truth for "which record is current" — see the file-level doc comment)
  is still true, regardless of the record's own status byte or current
  CRC validity — GC's job is to preserve the current record for its
  address, not to eagerly judge its data integrity; that's deferred to
  `eeprom_read()`'s lazy CRC check everywhere else in this file. The
  removed status-byte check was also gating a `g_lookup[addr]` array
  access on an `addr` value loaded directly from the (possibly
  corrupted) record header with no bounds check at all — replaced with
  an explicit `addr <= EEPROM_MAX_VIRTUAL_ADDR` check, closing what was
  a latent out-of-bounds read risk independent of this bug.

- **Both `scan_sector_records()`'s per-record header read and
  `read_sector_header()`'s per-sector header read gave up immediately on
  a single `flash_read()` failure**, treating a transient I/O glitch
  identically to a genuinely broken/corrupt header. For the record case,
  that meant discarding every record physically written *after* it in
  the same sector from `g_lookup` — even perfectly intact ones — the
  moment that sector was next rescanned. For the sector case, it meant
  `scan_all_sectors()` erasing an otherwise-healthy sector outright,
  including the `ACTIVE` one holding live application data, on nothing
  more than one flaky read. This is exactly the distinction
  `validate_record()`'s own doc comment already draws (a failed read
  means the I/O attempt faulted, not that the data is bad) — the two
  scan paths just weren't applying it. Fixed with a bounded retry
  (`SCAN_HEADER_READ_RETRIES = 3`) on both reads before falling back to
  the original handling.

All three fixes verified against the existing 21-test suite (still
21/21, zero new warnings under `-Wall -Wextra -Werror -Wpedantic`) and
against the specific stress-harness seeds/configurations that found each
one, which have permanent, deterministic regression pins of their own
alongside the AUDIT-2/3/4 tests added here.

**Found, not fixed here — open, tracked for a follow-up pass:**

- A bit flip (or torn partial write) landing on a record's own `size`
  header byte (rather than its status byte, which the fix above already
  covers) can still make `scan_sector_records()` lose every record
  written after it in the same sector: with `size` itself untrustworthy,
  the record's length becomes genuinely undecodable, and skipping
  forward by a wrong amount would be its own hazard. This is the one
  case the record-header fix above deliberately left as a hard stop.
  Closing it properly likely needs either a resync scan (probe forward
  looking for the next structurally-and-CRC-plausible header) or a
  redundant length encoding — a larger design change than a targeted bug
  fix, out of scope here.
- Sustained `EEPROM_WRITE_FAILED`/`EEPROM_ERASE_FAILED` during sector
  rotation (`ensure_room()` retiring the active sector, then
  `activate_sector()` failing to promote its replacement) can cascade
  into `EEPROM_FORMAT_ERROR` on a later `eeprom_init()` once enough
  sectors end up stuck in an inconsistent state, and/or leave a
  `SECTOR_STATE_GC_DEST` sector permanently stranded from an aborted
  `eeprom_garbage_collect()` call (`compact_sector()` failing partway
  through, before the source sector's erase). A later manual GC call can
  then trip over that stranded state and return `EEPROM_SECTOR_FULL` —
  which `eeprom_garbage_collect()`'s own doc comment does not list as a
  possible return value, a real doc/implementation mismatch on top of
  the underlying robustness gap. Reproducible with `WRITE_FAIL` alone
  (no other fault type involved) within a few hundred operations at
  realistic fault rates; needs a proper design pass on rotation/GC
  interruption recovery, not a quick patch.

## Verification performed

No target toolchain was available in the environment this was written in, so
correctness was verified on the host instead of on real hardware:

- `make run` (see [Makefile](Makefile)): builds `src/eeprom.c` +
  `test/test_eeprom.c` with `gcc -std=c11 -Wall -Wextra -Werror -Wpedantic`
  and runs all 21 test cases (the original 20 plus `AUDIT-1`, the Claim-1
  regression test above) against a RAM-backed mock Flash. **Result: 21/21
  pass, zero compiler warnings.**
- Same build rerun with `-fsanitize=address,undefined
  -fno-sanitize-recover=all`. **Result: 21/21 pass, no ASan/UBSan reports**
  (no buffer overflows, no undefined behavior) across the stress tests
  (1000 sequential overwrites, 500 random-address writes) and the
  corruption/power-loss simulations.
- `cppcheck --addon=misra` (a Windows build, 2.21.0, located and run
  directly since it wasn't on `PATH`) was run against `src/eeprom.c` — see
  "MISRA-C:2012 Deviation Record" below for the full command, results, and
  the four documented Advisory-rule deviations. Zero Required or Mandatory
  violations. This supersedes the earlier "no analyzer available" note
  that used to be here; the rules listed in EEPROM_SPEC.md §9 were
  originally followed by hand only (see the manual checklist below), and
  are now tool-verified as well. Other analyzers (PC-lint, PRQA, etc.)
  still haven't been run and would be worth a second opinion before
  shipping.
- Not tested: real STM32 hardware / HAL flash driver, actual timing
  (<10ms write / <5ms read / <100ms init budgets from spec §3), and the
  ECC-Flash caveat noted above.

## Key design decisions

**Record address field widened to `uint16_t`.** The spec's example record
table shows a 1-byte virtual address, but also explicitly notes "if >= 256
addresses needed, use uint16_t" — and the address space is defined as
0x0000-0x0FFF (4096 values), so the 2-byte form is what the spec itself
calls for. The record header is `status(1) + addr_lo(1) + addr_hi(1) +
size(1)` = 4 bytes, still landing data at offset 4 as shown in the spec.

**Sector header resolved in favor of §4.2 over the task prompt's
"SECTOR HEADER FORMAT (EXACT)" section.** These two sources disagree: §4.2
(the authoritative spec) packs sequence/state/write-pointer into a single
32-bit word (8/8/16 bits) plus a separate 4-byte erase count; the task
prompt's condensed table instead says "sequence: 4 bytes" *and* "state: 4
bits" *and* "write pointer: 12 bits" all within the same 4 bytes, which is
internally inconsistent (that's 32+4+12 bits in a 32-bit word). Rather than
propagate that inconsistency, this implementation uses its own 16-byte
header: `sequence(4) + state(1) + reserved(3) + erase_count(4) +
reserved(4)`, with each multi-byte field individually aligned. A 1-byte
sequence field (as an 8/8/16 packing would imply) would wrap after 256 GC
cycles, which is not viable for a "production automotive firmware" target;
a full 4-byte sequence avoids that.

**Sector free space is never persisted as a running write pointer.** Real
NOR Flash can only clear bits between erases (the test harness's mock flash
models this too: `mock_flash[i] &= data[i]`). A write pointer that grows on
every single record append would need to be rewritten with a larger value
on every write, which requires *setting* bits — impossible without a
sector erase. Instead, free space is always recovered by scanning forward
from the header until the first fully-erased (`0xFF 0xFF 0xFF 0xFF`) record
slot is found. This is unambiguous because a legitimate record's address
high byte is always `<= 0x0F` (the address space tops out at 0x0FFF), so an
all-0xFF header can never be a real record — it can only mean "free space
starts here." This scan doubles as the corruption/partial-write detector
(see below) and is exactly the mechanism the spec itself prescribes for
`build_lookup_table()` and `scan_all_sectors()`.

**Sector states are encoded to only ever clear bits.**
`EMPTY=0xFF -> ACTIVE=0xFE -> FULL=0xFC`, so every transition is a single,
erase-free Flash write, and the transitions are monotonic (a sector can
never accidentally look "less used" than it did before without a real
erase).

**The RAM lookup table (`g_lookup`), not the on-flash status byte, is the
source of truth for "which record is current."** `write_record()` does
best-effort invalidate the superseded record's status byte, but if that
particular write fails or is skipped, nothing breaks: `scan_sector_records()`
rebuilds the lookup table purely by walking records in flash log order and
letting the *last* one scanned for a given address win, which reproduces
"latest write wins" regardless of status-byte state. `compact_sector()`
uses the same authority check (`status==VALID && g_lookup[addr]==this
record's own location`) to decide what survives a GC pass.

**Garbage collection destination**: prefers an idle EMPTY sector when one
exists (guaranteed to fit the reclaimed sector's survivors, since they never
exceeded one sector's capacity to begin with — they came from a sector of
the same size), claimed via `mark_gc_destination()` into `SECTOR_STATE_GC_DEST`
— **not** `SECTOR_STATE_ACTIVE`, which is reserved exclusively for "the
sector application writes currently go to" (see the audit-fix section
above for why conflating the two is a critical bug). Falls back to
compacting into the *current active* sector only when no EMPTY sector is
available; that fallback is only invoked automatically, immediately after
a fresh promotion (see `ensure_room()`), when the active sector is
guaranteed to be nearly empty and therefore has room. This combination is
what makes the design correct even at the minimum `num_sectors = 2`
(classic two-page EEPROM emulation, à la ST's original app note) as well
as at `num_sectors = 3` (the spec's own example config, and what most
tests use) and above. An earlier draft always targeted the active sector
as GC destination; that broke a manually-invoked `eeprom_garbage_collect()`
call in testing (TC-009) once the active sector had already absorbed some
of its own writes and no longer had guaranteed headroom for an entire
reclaimed sector's worth of survivors — fixed by adding the
EMPTY-destination preference. A *second* draft used `activate_sector()`
(state `ACTIVE`) for that EMPTY destination — the critical bug the audit
found and `GC_DEST` fixes; see above.

**Wear-leveling is round-robin by construction**, not by an explicit
"next sector" pointer: `ensure_room()` always promotes *some* EMPTY sector
via `find_empty_sector()`, and `perform_garbage_collection()` always
reclaims the *oldest* FULL sector (lowest sequence number) first. Combined,
every sector cycles through EMPTY -> ACTIVE -> FULL -> (erased) -> EMPTY at
roughly the same rate, which TC-008 checks directly (erase counts across
sectors differ by at most 1 after sustained writes).

## Error-code mapping for cases the spec's enum doesn't cover

`eeprom_status_t` has no dedicated "invalid configuration" or "null
argument" code. Where the spec doesn't specify a code:

- `eeprom_init()` with a null/inconsistent `eeprom_config_t*` (missing
  callbacks, zero sector size, `num_sectors` outside `[2, 8]`, unsupported
  `write_width`) returns `EEPROM_FORMAT_ERROR` — "the configured Flash
  layout is not usable" is the closest existing fit.
- `eeprom_write()`/`eeprom_read()` with a null data/size pointer return
  `EEPROM_INVALID_SIZE` (bundled with the `size == 0` check they already
  need).
- `eeprom_get_stats()` with a null `stats` pointer returns
  `EEPROM_FORMAT_ERROR` for lack of a better fit.
- Flash read failures (the config struct's `flash_read` returning nonzero)
  now have a dedicated code, `EEPROM_READ_FAILED` (added post-audit — see
  the findings section above), returned by `eeprom_read()` when either the
  record header/CRC read or the final data read fails. During
  `eeprom_init()`'s sector scanning, a read failure is still treated as
  sector corruption (same recovery path as an inconsistent header) rather
  than surfaced as `EEPROM_READ_FAILED`, since `scan_all_sectors()`/
  `scan_sector_records()` have no caller to report a status code to at that
  point — recovering conservatively (erase and mark EMPTY) is the only
  option available during init regardless of *why* the header looked wrong.

## Test-case notes / deviations from the literal test text

- **TC-006** (256/1000-byte writes rejected): `eeprom_write()`'s `size`
  parameter is `uint8_t` per the spec's exact function signature, so a
  256- or 1000-byte request is not representable at the call site at all —
  the type system enforces the upper bound at compile time, which is a
  stronger guarantee than a runtime check. The test exercises the one
  representable invalid value, `size == 0`.
- **TC-009/TC-012/TC-016**: use a 1KB-sector config (`make_small_config()`
  in the test file) instead of the spec's 64KB example, so that the
  prescribed write counts (30, 1000, 50+) actually drive sector rotations
  and GC cycles within a fast, deterministic test run. With 64KB sectors
  none of those write counts fill even one sector, so assertions like
  "gc_runs > 0" would be vacuous.
- **TC-014** ("set sector state to PARTIAL"): this implementation has no
  persisted PARTIAL state (see the free-space design note above — a broken
  tail is *detected* by scanning, not flagged by a stored byte). The test
  instead hand-crafts a structurally-invalid record header (out-of-range
  address) directly after a legitimate record, which is what a real
  power-loss mid-write would plausibly leave behind, and verifies the
  library discovers it during `eeprom_init()`, discards it, and remains
  fully operational.
- **TC-020** (MISRA/compiler compliance): validated by the build itself
  (`-Wall -Wextra -Werror -Wpedantic`, zero warnings) rather than by a
  runtime check — there is nothing to execute for this one.

## MISRA-C:2012 checklist (manual, pre-tool reasoning)

Written before a static analyzer was run against the code (see the
tool-verified deviation record below, which supersedes the one item here
it overlaps with — the "known deviation" note on Rule 15.5).

- Rule 2.2 (no unreachable code): no `return`/`break` followed by dead code.
- Rule 6.1 (bit-fields only on unsigned types): no bit-fields used at all —
  sector/record header fields are packed and unpacked byte-by-byte via
  `load_u16/32`/`store_u16/32` instead, which is also portable across
  host/target endianness.
- Rule 10.x (implicit conversions): explicit `(uint8_t)`/`(uint16_t)`/
  `(uint32_t)` casts are used at narrowing points throughout; array
  indexing by a signed `int8_t` sector index is always written as
  `g_sectors[(uint8_t)idx]`.
- Rule 11.3 (no pointer/integer cast): none present; all Flash addressing
  is `uint32_t` arithmetic passed through the callback API, never cast
  to/from an actual pointer.
- Rule 14.3 (no constant-condition tests): none.
- Rule 17.5 / no unbounded recursion: no recursion anywhere in the library;
  `ensure_room()`'s "promote, then eagerly GC once if needed" is a
  straight-line sequence, not a loop or recursive retry.
- Rule 20.7 (macros fully parenthesized): all `#define` constants are
  parenthesized single values (e.g. `((uint8_t)0xFFU)`); no function-like
  macros are used at all (helpers are `static` functions instead).
- No `goto`; no function pointers other than the three config callbacks;
  const-correct API (`eeprom_config_t*` is `const` in `eeprom_init`, data
  pointers are `const` on the write path).

## MISRA-C:2012 Deviation Record (cppcheck --addon=misra)

Unlike the section above, this one reflects an actual static-analysis run,
not manual reasoning, and is the authoritative compliance statement for
this library (the one referenced from README.md). Re-run this yourself
with:

```sh
cppcheck --addon=misra --enable=all --inconclusive -I inc src/eeprom.c
```

The same command runs in CI on every push (see
`.github/workflows/ci.yml`), gated by `cppcheck-suppressions.txt`, which
suppresses only the four rule/file combinations documented below — any
*other* MISRA finding, or a finding against any rule not in that list,
fails the build. `eeprom.h` (declarations only) and `test/test_eeprom.c`
(explicitly host-only test code, not part of the shipped library — see
its own file header comment) are intentionally out of scope for this scan,
matching how the library actually ships: as `eeprom.c` compiled into a
downstream firmware project that this analysis cannot see.

The CI job doesn't gate on cppcheck's own `--error-exitcode`: `--enable=all`
is required for the misra addon to produce any `misra-c2012-*` output at
all with the cppcheck build tested (verified locally — with just
`--addon=misra --inconclusive`, zero MISRA findings were reported; adding
`--enable=all` is what makes them appear), but it also turns on unrelated
native cppcheck checks like `unusedFunction` that have nothing to do with
MISRA and aren't part of this deviation record. So the CI step instead
captures cppcheck's full output and greps it specifically for
`misra-c2012-` after suppression has already removed the four accepted
rule categories — anything left is, by definition, not on this list.
Since this codebase currently has zero Required/Mandatory violations (see
below), an unsuppressed finding today means either a Required/Mandatory
violation or a newly-introduced Advisory one nobody has reviewed yet —
either way, a human needs to look at it (fix the code, or extend both this
table and `cppcheck-suppressions.txt` deliberately) rather than have it
silently pass or silently fail the build for the wrong reason.

**CI incident, 2026-08-15**: the workflow initially used
`--error-exitcode=1` directly, and separately failed in CI with
`cppcheck: error: Failed to add suppression. No id.` — a crash while
*parsing the suppression list itself*, before any analysis of `eeprom.c`
ran at all. This could not be reproduced against a locally available
cppcheck 2.21.0, which parsed the same file (at the time, containing `#`
comment lines, including one ending in a literal `\`, meant purely as
human-readable documentation) without issue — strongly suggesting a
suppression-list-parser behavior difference against whatever cppcheck
version the GitHub-hosted Ubuntu runner's `apt-get install cppcheck`
resolved to, most plausibly around comment-line handling (a trailing `\`
inside what's meant to be an inert `#` comment is exactly the kind of
construct a stricter or older line parser can misinterpret as a
continuation marker). Rather than chase the exact version boundary,
`cppcheck-suppressions.txt` was rewritten to the minimal, maximally
portable format the feature has supported since its introduction: bare
`[error id]:[filename]` lines, no comments, no blank lines - the
human-readable justification for each entry lives here instead, which is
where the CI workflow's own comments point readers who want it. Verified
locally (both that this minimal file still suppresses exactly the four
expected rule/occurrence combinations, and that removing one entry from a
copy of the file correctly makes the grep-based CI check fail) before
relying on it. The `--error-exitcode=1`-vs-`unusedFunction` interaction
described in the paragraph above was found and fixed in the same pass.

**CI incident, 2026-08-16**: a later push failed the MISRA job with
`src/eeprom.c:-1:0: information: Unmatched suppression: misra-c2012-8.7
[unmatchedSuppression]`. Investigated before touching anything: `git
blame cppcheck-suppressions.txt` showed the `misra-c2012-8.7:src/eeprom.c`
entry unchanged since the file's introduction, and re-running cppcheck
2.22 against every one of the preceding commits' exact `src/eeprom.c` —
with `--suppressions-list` deliberately omitted, so nothing could hide —
confirmed all 7 flagged functions (the public API, non-`static` because
they're declared in `eeprom.h` and called from outside this translation
unit) still trigger a genuine Rule 8.7 finding at every commit, unchanged.
Re-adding `--suppressions-list=cppcheck-suppressions.txt` against the
same code, same cppcheck build, correctly suppressed all 7 with no
complaint. So the suppression entry itself was correct and untouched the
whole time — the finding never went away, and the file that's supposed to
hide it does, locally. The `information: Unmatched suppression` message
is best explained as an addon-vs-native suppression-bookkeeping quirk
specific to whatever cppcheck version the GitHub-hosted runner's
`apt-get install cppcheck` resolved to this time (the addon-generated
MISRA findings arrive via a subprocess, and don't always register as
"matched" against `--suppressions-list` the same way across cppcheck
versions) — the same category of local-vs-CI version difference as the
2026-08-15 incident above, just manifesting differently. The actual bug
was in the CI gate, not the suppressions file: `Unmatched suppression:
misra-c2012-8.7` contains the substring `misra-c2012-8.7`, so the
original plain `grep -q "misra-c2012-"` failed the build on an
*informational* notice about the suppression mechanism itself, not on
any code violation. Fixed by filtering out lines containing
`unmatchedSuppression` before that grep runs (see the workflow file's own
comment at that step for the one-paragraph version) — a real, unsuppressed
`misra-c2012-*` finding is always reported at `style:` or `error:`
severity, never wrapped in `[unmatchedSuppression]`, so this doesn't
weaken the gate against genuine new findings.

**Result: zero Required or Mandatory rule violations.** Every finding below
is Advisory, each is a deliberate, load-bearing design choice (not an
oversight), and each is scoped to specific, named, enumerable locations —
not a blanket "MISRA mode off" suppression.

| Rule | Category | Occurrences | Where | Verdict |
|------|----------|-------------|-------|---------|
| 8.9  | Advisory | 3  | `g_write_record_buf`, `g_compact_sector_buf`, `g_write_record_crc_input` | Accepted deviation |
| 15.4 | Advisory | 1  | `scan_sector_records()` | Accepted deviation |
| 15.5 | Advisory | ~50 (one per early `return`) | Throughout | Accepted deviation |
| 8.7  | Advisory | 7  | The 7 public API functions | Not a real issue (analysis-scope artifact) |

**Rule 8.9** ("An object should be defined at block scope if its
identifier only appears in a single function"): `g_write_record_buf` and
`g_write_record_crc_input` are used only by `write_record()`;
`g_compact_sector_buf` only by `compact_sector()` — each is textbook 8.9
material taken purely on scope grounds. They were deliberately promoted
from local (block-scope) variables to `static` file-scope storage
specifically to remove them from the stack — see the "Stack usage" and
"Independent audit findings" sections above for the full reasoning
(~800 bytes of worst-case depth removed from the `eeprom_write() -> ... ->
compact_sector()` call chain, a real overflow risk against a 1 KiB
Cortex-M task stack). Rule 8.9 doesn't have visibility into *why* an
object needed broader scope, only that it did — the deviation is the
correct tradeoff here, not a fix to apply. (Note: this is unrelated to
linkage — these objects are `static`, i.e. internal linkage already; 8.9
is purely about scope, block vs. file.)

**Rule 15.4** ("A project should not contain more than one break or goto
statement used to terminate a loop"): `scan_sector_records()`'s single
scan loop has three `break` statements, each terminating the loop for a
distinct, semantically necessary reason: a Flash read failure (line
~702-705), a fully-erased record slot meaning "free space starts here"
(line ~706-708), and a structurally invalid record meaning "corrupted /
partial tail" (line ~724-728). Collapsing these into a single exit point
would require an extra state variable threaded through the loop purely to
satisfy the rule, adding a layer of indirection without reducing the
actual complexity the three distinct outcomes represent — the "fix" would
make the function harder to read, not easier to verify.

**Rule 15.5** ("A function should have a single point of exit at the end"):
confirmed via cppcheck as roughly 50 early-return guard clauses across the
file. This is the intentional, project-wide coding convention already
documented under "Known deviation" in the manual checklist above:
functions with several independent validity checks (`eeprom_write()`,
`eeprom_read()`, `eeprom_init()`, etc.) return as soon as a check fails.
Every early return is a guard clause with no resource to release and no
cleanup to skip, so it carries none of the risk (missed `free()`/unlock/
etc.) that motivates the rule in languages or codebases that do have such
cleanup. This is one of the most common, widely-accepted MISRA C Advisory
deviations in defensive embedded C, not specific to this codebase.

**Rule 8.7** ("Functions and objects should not be defined with external
linkage if they are referenced in only one translation unit"): flagged for
all 7 functions declared in `eeprom.h` — `eeprom_init`, `eeprom_write`,
`eeprom_read`, `eeprom_exists`, `eeprom_get_stats`,
`eeprom_garbage_collect`, `eeprom_format`. This is a scope limitation of
the analysis, not a real issue: cppcheck was invoked against `src/eeprom.c`
alone, so it cannot see that these functions are declared non-static
specifically because they *are* the library's public API (that's the
entire point of `eeprom.h` existing), called from `test/test_eeprom.c` in
this repo and, in real use, from firmware in a separate translation unit
entirely — code cppcheck has no way to know exists when scanning this file
in isolation. Making these `static` would break the library. Treated as a
deviation here (suppressed in CI) rather than "fixed," since there is no
fix that doesn't break the API.

## Stack usage

Post-audit (see findings above), `write_record()`'s and `compact_sector()`'s
~260-byte working buffers are `static`, not stack-allocated, so they no
longer contribute to call-chain stack depth at all. `validate_record()`
still uses a stack-local buffer of up to `3 + 255 = 258` bytes plus a
257-byte body buffer (~515 bytes total) — kept on the stack deliberately,
because it's shared with the ISR-safe `eeprom_read()` path and must not be
static (see the audit findings section). Worst-case stack depth is now
dominated by whichever single function is deepest at the point
`validate_record()` is called (e.g. `eeprom_write() -> ... ->
compact_sector() -> validate_record()`), on the order of 550-650 bytes
rather than the pre-audit ~800+. Still worth checking explicitly against
the stack budget on the smallest F1 targets before deployment, and note
that `eeprom_read()` calling `validate_record()` from an ISR adds that
same ~515 bytes to the ISR's own stack requirement.

## Known limitations

- `num_sectors` must be >= 2 (enforced in `eeprom_init()`, returns
  `EEPROM_FORMAT_ERROR` otherwise); the design does not support a
  single-sector configuration (no possible GC destination).
- A sector whose *header* is detected as corrupt is erased immediately and
  unconditionally during `eeprom_init()` — any records it held are lost.
  This matches spec §7.2's prescribed recovery ("erase sector, mark as
  EMPTY") but means header corruption is not partially recoverable; only
  corruption confined to individual *records* (caught by CRC) is recovered
  record-by-record.
- After a header-corruption recovery, that sector's lifetime `erase_count`
  resets to 0 (the true prior count, if any, is unrecoverable once the
  header itself is untrustworthy) — a cosmetic accuracy loss in the
  diagnostics-only statistics, not a correctness issue.
- Total live (non-superseded) data across the whole virtual address space
  is implicitly bounded by one sector's capacity, since GC always compacts
  a single sector's survivors into one destination sector. This is the
  standard assumption behind this style of design (matches, e.g., ST's
  original 2-page EEPROM emulation app note) and should be accounted for
  when sizing `sector_size` against expected usage.
- Timing budgets from spec §3 (<10ms write, <5ms read, <100ms init) were
  not measured — they depend on the real `flash_read`/`flash_write`/
  `flash_erase` callback implementations and target clock speed, neither of
  which exist in this host-only environment.
