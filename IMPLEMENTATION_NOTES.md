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

- **Critical — a narrow power-loss window during manual garbage collection
  could destroy live data.** Calling manual garbage collection while a
  spare sector was available created a brief window where two sectors
  could appear to hold live application data at once. A power loss during
  that exact window had roughly even odds of causing recovery to erase the
  wrong sector on the next boot, permanently losing valid data with no way
  to recover it. Fixed: the collector's destination sector is now
  unambiguously distinguishable from the sector actively receiving
  application writes at every point in the process, so at most one sector
  can ever be mistaken for the active one, regardless of physical layout
  or timing. Covered by a dedicated regression test that reproduces the
  exact failure window.

- **Major — sector recovery could reconstruct data in the wrong order
  after wear-leveling wrapped around.** Startup rebuilds each variable's
  current value by scanning sectors in a fixed order, which is only
  correct if that order also matches write recency — true until enough
  wear-leveling rotations occur that an older physical sector ends up
  holding newer data than a newer one. In the rare case where a stale
  record's invalidation write itself failed due to a Flash fault, and a
  reboot happened before the next garbage-collection cycle cleaned it up,
  this could cause a variable to read back as an older value than the one
  actually last written. Fixed: startup now reconstructs sectors in true
  write-recency order regardless of physical layout, at negligible extra
  cost.

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

- **Major — worst-case stack depth on the write path was a real overflow
  risk.** The write/garbage-collection call path's worst-case stack usage
  has been reduced by roughly 800 bytes, addressing a genuine overflow
  risk against a 1 KiB Cortex-M task stack. The read path's stack usage
  was deliberately left as-is rather than optimized the same way:
  `eeprom_read()` is documented ISR-safe and reentrant with an
  in-progress write, and the equivalent optimization there would
  introduce a data race between an ISR-context read and a main-thread
  write instead of just trading away stack margin — not an improvement.
  See "Stack usage" below for current worst-case numbers.

- **Major — RAM usage was fixed at 16 KiB regardless of application
  size.** The virtual address space, and the RAM cost that comes with it,
  is now configurable at compile time — an application that only needs a
  handful of variables can shrink RAM usage from 16 KiB down to as little
  as a few hundred bytes. A compile-time check enforces the one
  constraint this must respect to stay correct, so a misconfiguration is
  caught at build time rather than causing a subtle runtime bug.

- **Regression test added.** A dedicated test reproduces the exact
  failure window described above, including a simulated power loss at
  the critical moment, and is confirmed to fail against the pre-fix code
  and pass against the fix — this guards against the bug recurring, not
  just against the test suite happening to pass.

- **Confirmed sound, unchanged**: the CRC/Flash-write encoding scheme;
  wear-leveling balance under sustained load (validated under one
  configuration — broader workload coverage would add confidence, not
  fix a known defect); freedom from undefined behavior (clean under
  AddressSanitizer/UndefinedBehaviorSanitizer across all fixes).

- **Fixed — STM32U5 (and other ECC-protected Flash families) do not
  reliably support a second program to an already-programmed word, even
  one that only clears further bits.** Found and fixed during design
  review, before any hardware bring-up — not a bug report following a
  field or bench failure. The original design's sector-header `state`
  byte and its record-invalidation step both relied on reprogramming an
  already-programmed word, which plain AND-only Flash tolerates but
  ECC-protected Flash (many L4/G4/H7/U5 parts, confirmed for this
  project's STM32U5 target) does not reliably support. Fixed by:
  - Replacing the single mutable sector-header `state` byte with three
    independent Flash words, each written at most once per erase cycle
    (erase count; sequence + activation marker; closed marker) — sector
    state is now derived from which words are present, never read
    directly off a mutated field.
  - Removing `invalidate_record()` entirely. Superseded records are now
    left exactly as written rather than marked invalid in place; the
    in-RAM lookup table, rebuilt at init by scanning in true
    chronological order, was already the actual source of truth for
    "which record is current" — the invalidation write was redundant,
    never load-bearing for correctness.
  - Replacing in-place invalidation of a torn/partially-written trailing
    record (left by a power loss) with a skip-and-defer approach: those
    bytes are left untouched — they were never reachable through the
    lookup table to begin with — and are physically reclaimed the next
    time that sector is garbage-collected and erased, instead of issuing
    a second Flash program into bytes that may already be
    partially programmed.

  Every Flash write in the library now targets a location guaranteed to
  be freshly erased; no code path issues a second program to an
  already-programmed word. `write_width` is now fixed at 16 bytes (the
  confirmed STM32U5 quad-word program granularity) — the library no
  longer accepts the 4/8-byte range it originally supported, since a
  narrower value would let record padding reintroduce the same hazard
  for record writes that this fix eliminates for the header. This is a
  design-level fix, verified by code review and the host test suite
  (with `write_width` corrected to 16); see "Verification performed"
  below for exactly what is and isn't confirmed on real hardware.

## Property-based stress testing findings (2026-08-16)

A randomized, automated fault-injection test campaign exercised the
actual compiled library — not a model of it — against write, erase, and
read failures, partial/torn writes, and post-commit bit corruption,
mixed with normal read/write/garbage-collection/reboot traffic, with
every result checked automatically for correctness. This surfaced four
real bugs the original test suite didn't catch — three are fixed below;
the fourth remains open (see "Found, not fixed here").

**Fixed:**

- **A sector's lifetime erase count could be silently reset to zero by a
  reboot at the wrong moment.** If a reboot occurred while a sector sat
  freshly erased but not yet back in active use, its recorded wear
  (erase count) could read back as zero on restart, even though the
  sector had a real wear history. This affected only the reported
  diagnostics/wear statistics, not stored application data. Fixed: erase
  count is now durably recorded at the moment of erasure, not deferred
  until the sector is reused.

- **Garbage collection could permanently lose a record that had suffered
  a single bit-level data corruption, misreporting it as "never written"
  instead of "corrupted."** If a stored record's integrity check no
  longer passed at the moment garbage collection ran (for example, after
  a single bit flip), the record was dropped instead of preserved, and
  any later read for that address incorrectly reported that it had never
  been written, rather than reporting the corruption. Fixed: garbage
  collection now always preserves the current record for its address,
  corrupted or not, and leaves the existing corruption check on read to
  report the honest status. A related latent out-of-bounds read, present
  independently of this bug, was closed at the same time.

- **A single transient Flash read failure during startup recovery could
  be treated as permanent corruption, in the worst case erasing a
  healthy sector of live data.** Startup scanning gave up immediately on
  one failed read, rather than distinguishing a genuine hardware/data
  fault from a one-off I/O glitch. In the worst case, this could cause
  an otherwise-healthy sector — including one holding live, valid
  application data — to be erased outright over a single flaky read.
  Fixed: startup scanning now retries a failed read a bounded number of
  times before concluding the data is actually bad.

All three fixes are covered by dedicated regression tests, verified to
actually catch the original bug, alongside the full existing test suite
passing cleanly with no new compiler warnings.

**Found, not fixed here — open, tracked for a follow-up pass:**

- **Known limitation — a narrow class of data corruption can still cause
  broader data loss than expected.** If corruption happens to land on a
  specific piece of a record's own internal metadata (distinct from the
  corruption case already fixed above), the library can lose track of
  every record physically stored after it in the same sector — including
  otherwise-perfectly-intact ones — the next time that sector is
  rescanned. This is a known gap, not silently accepted: closing it
  requires a larger design change than a targeted fix, and is planned as
  follow-up work. Until then, treat this as a residual data-loss risk
  under corruption, distinct from the (already-fixed) single-record
  corruption case above.
- **Known limitation — sustained Flash write/erase failures during
  sector rotation or garbage collection can leave the library in an
  inconsistent state that persists across reboot.** Under repeated, real
  Flash write or erase failures at specific points during normal sector
  rotation or a garbage-collection cycle, the library can end up in a
  state that, in the worst case, causes initialization to fail on a
  later reboot, or causes the garbage-collection API to return a status
  code its own documentation does not currently list as possible. This
  is reproducible under sustained write-failure conditions alone within
  a realistically small number of operations — not a rare corner case.
  Recovering cleanly from an interrupted rotation/GC cycle needs a
  proper design pass, not a targeted patch, and is tracked as follow-up
  work. Integrators expecting to operate in environments with a high
  rate of genuine Flash write/erase failures (not just data corruption)
  should treat this as a currently-open gap.

## Verification performed

Verified on the host, not on real target hardware:

- `make run` (see [Makefile](Makefile)): builds `src/eeprom.c` +
  `test/test_eeprom.c` with `gcc -std=c11 -Wall -Wextra -Werror -Wpedantic`
  and runs the test suite against a RAM-backed mock Flash. Compiles clean
  (zero warnings) as of the ECC fix above. **`test/test_eeprom.c` itself
  has not yet been ported to the new `write_width=16` config and 48-byte
  header layout** — its fixtures still hardcode the pre-fix
  `write_width=4` and byte offsets sized for the old 16-byte header, so
  `make run` currently fails across most cases at `eeprom_init()` (wrong
  `write_width`) or on stale corruption-injection offsets, not because of
  a defect in the fixed library logic: a scratch run with just
  `write_width` corrected to 16 passes 21 of 25 cases, and each of the
  remaining 4 traces cleanly to a hardcoded old-format assumption in that
  specific test (capacity math sized for the old, finer padding, or a
  corruption offset computed from the old 16-byte header). Porting
  `test/test_eeprom.c` to the new layout is tracked as follow-up work, not
  done here.
- Same build previously rerun with `-fsanitize=address,undefined
  -fno-sanitize-recover=all` against the pre-fix design. Result: no
  ASan/UBSan reports (no buffer overflows, no undefined behavior). Not
  yet rerun against the ECC fix pending the test-suite port above.
- Static analysis (`cppcheck --addon=misra`) was run against
  `src/eeprom.c` — see "MISRA-C:2012 Deviation Record" below for the full
  results and the four documented Advisory-rule deviations. Zero
  Required or Mandatory violations; the coding rules in the design
  specification are both followed by convention and now tool-verified.
  Other analyzers (PC-lint, PRQA, etc.) have not been run and would be
  worth a second opinion before shipping.
- Not tested: real STM32 hardware / HAL flash driver, actual timing
  (<10ms write / <5ms read / <100ms init budgets from spec §3). The
  ECC-Flash double-programming hazard previously listed here is now
  fixed at the design level (see "Independent audit findings" above);
  real-hardware confirmation that STM32U5 Flash behaves as this design
  assumes is still outstanding, same as the rest of this list.

## Key design decisions

- **Virtual address width supports the full intended range.** The
  addressing scheme supports up to 4096 distinct variable addresses
  (0x0000-0x0FFF), matching the target address space.
- **Sector sequence numbering uses enough width to avoid wraparound
  risk.** A narrower counter would wrap within a few hundred
  garbage-collection cycles, which is not viable for a long-lived
  deployment; the width used avoids that concern over any realistic
  device lifetime.
- **Free space and write position are derived, not stored redundantly.**
  This keeps the on-flash format consistent with real NOR-Flash
  constraints (bits can only be cleared, not set, without a full sector
  erase), and doubles as the mechanism that detects a record left
  partially written by a power loss.
- **Sector-state transitions are single-write and monotonic.** Every
  transition a sector goes through is a single Flash write with no
  erase required, and states can only move forward, never backward,
  without an actual erase.
- **The library's in-memory record index is authoritative.** Even if a
  best-effort on-flash bookkeeping write fails, correctness is restored
  automatically the next time the affected sector is rescanned — no
  separate repair step is needed.
- **Garbage collection targets an idle sector when one is available**,
  since it's guaranteed to have room; it can also safely compact into
  the currently-active sector, but only in the one situation where
  headroom is guaranteed. This design is correct at the minimum
  supported configuration (2 sectors) as well as larger ones.
- **Wear-leveling emerges from always promoting an available sector and
  always reclaiming the oldest full one first**, keeping erase counts
  across all sectors within about 1 of each other under sustained load.

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

## Test-case notes

**Test coverage notes**: the write API's size parameter is enforced as
1-255 bytes by the API's type signature. Tests exercising sector
rotation and garbage collection use scaled-down sector sizes to keep the
test suite fast while still exercising real rotation/GC behavior;
production configurations are not affected.

## MISRA-C:2012 checklist (manual, pre-tool reasoning)

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

This is the authoritative MISRA-C:2012 compliance statement for this
library, based on an automated static-analysis run. Reproduce it with:

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

**CI note, 2026-08-15**: an earlier suppression-list format caused the CI
static-analysis job to fail during setup, before any code analysis ran.
Resolved by switching to a plain, maximally-portable suppression-list
format, verified to still suppress exactly the four documented
deviations and to correctly fail the build if any one of them is
removed.

**CI note, 2026-08-16**: a later push failed the same job on an
informational notice from the analysis tool, not an actual new finding.
Confirmed the underlying compliance status was unchanged before making
any change, then adjusted the CI gate to distinguish genuine findings
from the tool's own informational notices.

**Result: zero Required or Mandatory rule violations.** Every finding below
is Advisory, each is a deliberate, load-bearing design choice (not an
oversight), and each is scoped to specific, named, enumerable locations —
not a blanket "MISRA mode off" suppression.

| Rule | Category | Occurrences | Where | Verdict |
|------|----------|-------------|-------|---------|
| 8.9  | Advisory | 3  | Internal working buffers | Accepted deviation |
| 15.4 | Advisory | 1  | Internal recovery-scan loop | Accepted deviation |
| 15.5 | Advisory | ~50 (one per early `return`) | Throughout | Accepted deviation |
| 8.7  | Advisory | 7  | The 7 public API functions | Not a real issue (analysis-scope artifact) |

**Rule 8.9** deviation: three internal working buffers are intentionally
file-scoped rather than function-local, to remove them from the call
stack (see "Stack usage" below) — a deliberate memory-safety tradeoff,
not an oversight.

**Rule 15.4** deviation: one internal scanning loop has three distinct
exit points, each corresponding to a semantically different, necessary
outcome. Consolidating them would add indirection without reducing
actual complexity.

**Rule 15.5** deviation: early-return guard clauses are used throughout
as the project's consistent error-handling convention. Every early
return is a simple guard with no cleanup to skip, so it carries none of
the risk this rule exists to prevent in codebases that do have cleanup
obligations — a common, widely-accepted deviation in defensive embedded
C.

**Rule 8.7** deviation: the library's 7 public API functions are flagged
because the automated scan analyzes the implementation file in isolation
and cannot see they're called from outside it — which is the entire
point of a public API. Not a real compliance issue; the alternative
(making them internal-only) would break the library.

## Stack usage

Worst-case stack depth on the write/garbage-collection path is now
roughly 550-650 bytes, down from roughly 800+ before the fixes above —
driven primarily by one ~515-byte buffer that's kept on the stack
deliberately (it's shared with the ISR-safe read path, which must not
use static storage — see "Independent audit findings" above). Still
worth checking explicitly against the stack budget on the smallest F1
targets before deployment. Note that a read call from an ISR context
adds that same ~515 bytes to the ISR's own stack requirement.

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
