# Non-Contiguous Untransmitted Sensor Buffer

## Problem

The current sensor flash buffer (`src/storage/sensor_flash_buffer.{h,cpp}`) tracks
untransmitted records with a single linear `read_index` cursor. After an ACK, the
caller advances `read_index` by `total_records_scanned`, so the cursor moves
forward in lockstep with the scan window.

This works while transmission failures are rare and uniform, but breaks down in
two real scenarios:

1. **Head-of-line stall.** If the record at `read_index` (or its enclosing
   batch) keeps NACK'ing — corrupt payload, hub-side rejection, repeated CRC
   error on a single record — `transmitBacklog()` re-sends the same first
   batch every cycle and never reaches newer records that *would* deliver.
   Newer untransmitted records pile up behind the stuck head until wraparound
   overwrites them.

2. **Sparse recovery.** After prolonged outage we may have legitimately
   non-contiguous gaps (records dropped due to flash CRC errors, or a future
   feature that lets us transmit "newer-first" during catch-up). The current
   structure cannot describe "records 100, 102, 105 are untransmitted; 101,
   103, 104 are sent" except by scanning the whole window record-by-record
   on every transmit attempt.

The per-byte `transmission_status` field (added in `flash_transmitted_flag.md`)
already gives us record-level truth. What's missing is a way to *use* it
efficiently when the untransmitted set is non-contiguous.

## Design

### Principle: per-record status is truth, `read_index` is a hint

Demote `read_index` from "everything before this is transmitted (or lost)" to
"first sector that may still contain untransmitted records". Per-record
`transmission_status` becomes the single source of truth for what to send.

This is a one-line semantic change but it unlocks non-contiguous tracking:
the flash already encodes the state we want; we just stop pretending the
cursor is authoritative.

### Per-sector untransmitted-count summary (RAM)

To keep scans bounded, maintain a RAM-resident summary of how many
untransmitted records live in each sector.

```cpp
// ~24K sectors × 2 bytes = 48KB on RP2350B (520KB SRAM, fits comfortably)
constexpr uint32_t SECTOR_COUNT = DATA_REGION_SIZE / SECTOR_SIZE;
uint16_t untransmitted_per_sector_[SECTOR_COUNT];
```

Sized at `uint16_t` because `RECORDS_PER_SECTOR ≈ 341` fits in 9 bits with
headroom. A `uint8_t` would technically work (max 341 > 255 — would need
saturating semantics; not worth the 24KB savings on this MCU).

Invariants:

- `untransmitted_per_sector_[s]` == count of records in sector `s` with
  `flags & RECORD_FLAG_VALID` and `transmission_status == 0xFF`
- Sum over all sectors == `getUntransmittedCount()`
- A sector with count 0 can be skipped entirely during scans

### Maintenance

| Operation        | Summary update                                            |
| ---------------- | --------------------------------------------------------- |
| `writeRecord`    | `++untransmitted_per_sector_[sector_of(write_index)]`     |
| `markTransmitted`| `--untransmitted_per_sector_[sector_of(index)]`           |
| Wraparound drop  | `--untransmitted_per_sector_[sector_of(overwritten)]` if the overwritten record was untransmitted (already detectable via existing read-before-overwrite) |
| Cold boot        | Rebuild during the existing `recoverReadIndex()` sector scan |
| Warm boot        | Either restore from PMU RAM or rebuild on next idle window |

All updates are single arithmetic operations on hot-path writers; no extra
flash I/O.

### Scan algorithm

Replace the linear scan in `readUntransmittedRecords()` with sector-skipping:

```text
sector = sector_of(read_index)
while batch_not_full and sector != sector_of(write_index):
    if untransmitted_per_sector_[sector] == 0:
        sector = (sector + 1) % SECTOR_COUNT  # whole sector skipped
        continue
    scan records in sector, collect those with transmission_status == 0xFF
    sector = (sector + 1) % SECTOR_COUNT
```

Worst-case work scales with *sectors that contain untransmitted records*, not
total record count. A sparse buffer with 50 untransmitted records spread
across 50 sectors costs ~50 sector reads instead of millions of record reads.

### `read_index` advancement

After a successful batch ACK:

1. `markTransmitted(index)` for each record in the batch (per-record, in any
   order — already supported by the NOR-friendly 0xFF→0x00 write).
2. Advance `read_index` past the *contiguous transmitted prefix*: walk
   forward sector-by-sector while `untransmitted_per_sector_[sector] == 0`,
   then within the first non-empty sector advance record-by-record until the
   first untransmitted one. This reclaims wraparound headroom without
   forgetting gaps in the middle.

This is the only behavioural change that removes the "advance by count"
pattern; gap records past the new `read_index` remain visible to the next
scan.

### Head-of-line stall mitigation (separate from the data structure work)

With non-contiguous tracking in place, the policy fix is straightforward:
on repeated batch failure, the transmitter can advance its internal
*cursor* past the failing batch (without modifying flash state) so the next
attempt picks up newer records. The skipped records remain
`transmission_status == 0xFF` in flash and will be revisited on the next
full pass. This is a `sensor_mode.cpp` change, not a buffer change, but it
only becomes safe once the scanner can find non-contiguous untransmitted
records efficiently.

Out of scope for this plan — captured here so the motivation is clear.

## Memory and Compatibility

- **RAM cost:** 48KB on RP2350B (v4). v3 (RP2040, 264KB SRAM) is tighter; if
  v3 still needs to support this, drop to per-chunk granularity (32 records
  per chunk → ~64K chunks × 1 byte = 64KB — actually worse) or per-sector
  `uint8_t` with saturation at 255 (24KB). Decision deferred until v3
  relevance is confirmed.
- **Flash format:** unchanged. No new fields, no migration. The plan reuses
  the existing `transmission_status` byte as truth.
- **PMU state:** add the summary array to the optional warm-boot restore
  payload, or accept the rebuild cost on warm boot. Rebuild is bounded by
  one full sector-scan pass (same cost as cold boot recovery), so accepting
  the rebuild keeps PMU coupling minimal.

## Implementation Steps

1. **Add summary array** — `uint16_t untransmitted_per_sector_[SECTOR_COUNT]`
   member of `SensorFlashBuffer`, initialized to zero in `init()`.
2. **Hook writers** — increment in `writeRecord()`, decrement in
   `markTransmitted()`, decrement on wraparound-overwrite of a record that
   was untransmitted.
3. **Build on cold boot** — extend `recoverReadIndex()` (or add a sibling
   pass) to populate the summary during the existing scan. No extra flash
   reads beyond what cold boot already does.
4. **Rewrite scanner** — replace the linear loop in
   `readUntransmittedRecords()` with the sector-skipping algorithm above.
5. **Rewrite advance** — replace `advanceReadIndex(count)` with
   `advanceReadIndexPastTransmittedPrefix()` that uses the summary to walk
   forward only over fully-transmitted sectors.
6. **Update callers** — `sensor_mode.cpp` ACK callback marks each record
   individually (it already does this in a loop) and calls the new advance
   function instead of passing `total_records_scanned`.
7. **Tests** — extend the integration test suite (`src/tests/`) with cases
   that produce non-contiguous transmitted/untransmitted patterns and
   verify the scanner returns the correct records and the advance logic
   reclaims only contiguous prefixes.

## Open Questions

- Do we want to surface the summary in stats/logs (e.g. "fragmentation =
  untransmitted_count / non_empty_sector_count") to help diagnose stall
  scenarios in the field?
- Warm boot: accept rebuild cost, or extend PMU RAM payload? Rebuild is
  simpler and cold-boot-equivalent; recommend accepting the cost unless
  warm-boot latency is measured to be a problem.
- v3 (RP2040) support: is this needed, or is the new buffering v4-only?
