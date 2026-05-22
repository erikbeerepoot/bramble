# Read Index Recovery Using Transmitted Flag

## Problem

On cold start, `recoverReadIndex()` tries to validate `read_index` from stale flash metadata. If invalid, it **resets read_index to write_index** — effectively declaring "nothing to read" and silently dropping all untransmitted records.

The transmitted flag (`transmission_status` byte) on each record is the true source of truth for what has/hasn't been sent. We should use it to find the correct read position instead of trusting metadata alone.

## Current Flow (cold start)

1. `init()` loads stale flash metadata (including `read_index`)
2. `scanForWriteIndex()` fixes `write_index` via binary search
3. `recoverReadIndex()`:
   - Validates `read_index` from metadata (CRC check on record at that position)
   - If invalid → resets to `write_index` (data loss!)
   - `fastForwardReadIndex()` advances past transmitted records

## Proposed Flow

After `scanForWriteIndex()` determines `write_index`, recover `read_index` by scanning for the **first valid, untransmitted record**:

1. **Scan backward from `write_index`** to find the start of the untransmitted region
   - Read only the 1-byte `transmission_status` field (efficient, no full record read)
   - Stop when we hit a transmitted record or the beginning of data
   - This gives us the earliest untransmitted record

2. **Set `read_index`** to that position

3. **During `readUntransmittedRecords()`** (already works this way):
   - Skip records with CRC errors (corrupted)
   - Skip records already marked as transmitted (safety net)
   - Collect valid, untransmitted records into the output buffer

## Design Details

### Why scan backward from write_index?

Records are written sequentially. Transmitted records are at the "old" end (near `read_index`), untransmitted at the "new" end (near `write_index`). Scanning backward from `write_index` finds the boundary efficiently — most of the time the untransmitted region is small (a few wake cycles' worth).

### Why not scan forward from 0?

Forward scan from 0 would work but is slower — it has to skip over potentially millions of transmitted records. Backward scan converges faster on the boundary.

### Efficiency

- Read only the 1-byte `transmission_status` at each index (same as `fastForwardReadIndex()`)
- Worst case: scan all records (cold start after long offline period with no transmissions)
- Typical case: scan a few hundred records (recent untransmitted backlog)

### What about the binary search approach?

We could binary search for the transmitted→untransmitted boundary, but:
- The transmitted flag is only 1 byte read per probe, already very fast
- The transition isn't guaranteed to be monotonic (hub may have ACK'd records out of order, or corruption could make a transmitted record look untransmitted)
- Linear backward scan is simple, correct, and fast enough

## Changes Required

1. **`recoverReadIndex()`** — Replace current logic with backward scan from `write_index`
2. **Remove `isReadIndexValid()`** — No longer needed; read_index is derived from transmitted flags
3. **Remove `fastForwardReadIndex()`** — Subsumed by the backward scan (it finds the exact boundary directly)
4. **`readUntransmittedRecords()`** — No changes needed (already skips transmitted + invalid)

## Edge Cases

- **All records transmitted**: backward scan reaches beginning of data → `read_index = write_index` (empty buffer, correct)
- **No records transmitted**: backward scan reaches beginning of data → `read_index = 0` (all records pending, correct)
- **Corrupted transmission_status byte**: A corrupted byte won't be exactly `0x00` (RECORD_TRANSMITTED), so the record appears untransmitted. This is safe — worst case we re-transmit, and the hub deduplicates via timestamp
- **Buffer wrap-around**: Same limitation as `scanForWriteIndex()` — deferred until wrap is realistic
