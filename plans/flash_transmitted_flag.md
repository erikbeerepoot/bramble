# Per-Record Transmitted Flag

## Problem

The `markTransmitted()` function exists but is never called. When `read_index` gets
out of sync (PMU state loss, cold start recovery), there's no per-record indicator
of whether data has been successfully ACK'd by the hub. This can cause duplicate
transmissions or, worse, data loss if `read_index` jumps past unsent records.

## Design

### NOR Flash Constraint

NOR flash can only clear bits (1 to 0) without a sector erase. The current
`markTransmitted()` does a full sector read-modify-write because setting
`RECORD_FLAG_TRANSMITTED` (0x01) in the `flags` byte requires a bit to go 0 to 1
(flags was written as 0x02). This is expensive and wears the flash.

### Solution: Use the `reserved` byte as `transmission_status`

Rename the `reserved` byte (offset 9 in the record) to `transmission_status`:

- **On write**: `transmission_status = 0xFF` (erased state = not transmitted)
- **On ACK**: Write `0x00` to `transmission_status` (clears all bits, no erase needed)
- **Check**: `transmission_status != 0xFF` means transmitted

This is a single-byte in-place write. NOR flash handles 0xFF to 0x00 natively.

### CRC Compatibility

The CRC covers bytes 0-9 (including `transmission_status`). To avoid CRC invalidation
when marking transmitted:

- `calculateRecordCRC()` normalizes `transmission_status` to 0x00 before calculating
- Old records already have `reserved = 0x00`, so their CRC is unchanged
- New untransmitted records have `transmission_status = 0xFF`, normalized to 0x00 for CRC
- Transmitted records have `transmission_status = 0x00`, matching normalization

This is fully backwards compatible with existing flash data.

### Changes

1. **sensor_data_record.h**: Rename `reserved` to `transmission_status`, add helpers
2. **sensor_flash_buffer.cpp**:
   - `calculateRecordCRC()`: Normalize `transmission_status` to 0x00
   - `writeRecord()`: Set `transmission_status = 0xFF`
   - `markTransmitted()`: Single-byte write instead of sector read-modify-write
   - `readUntransmittedRecords()`: Skip already-transmitted records
   - Remove `sector_buffer_` (no longer needed, saves 4KB RAM)
3. **sensor_mode.cpp**: Call `markTransmitted()` from ACK callback

### Read Pointer vs Per-Record Flag

The `read_index` pointer remains the primary mechanism. The per-record flag is a
safety net: if `read_index` is stale after recovery, `readUntransmittedRecords()`
skips already-transmitted records instead of re-sending them.
