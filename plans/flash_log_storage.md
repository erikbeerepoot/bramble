# Flash Log Storage

## Overview

Save debug logs (normally sent to UART/USB serial) to a reserved region of external flash, so they can be retrieved later for post-mortem debugging. Retrieval via USB mass storage mode.

## Feasibility

**Verdict: Fully feasible.** The hardware and software infrastructure already exists to support this.

- The MT25QL01GBBB external flash (128MB) is already present and has a working driver (`ExternalFlash`).
- The `SensorFlashBuffer` already implements a circular buffer pattern on this flash that we can reuse for logs.
- The `Logger` class has a clean extension point for adding a flash sink alongside the existing printf output.
- USB mass storage is achievable via TinyUSB (bundled in the Pico SDK) but requires replacing the default CDC-only USB stack with a composite CDC+MSC device.

## Design

### 1. External Flash Partitioning

The external flash currently dedicates nearly all 128MB to sensor data. The sensor buffer has capacity for ~12 years of data at 30-second intervals -- far more than needed. We can partition a portion for log storage with no practical impact on sensor capacity.

**Current layout:**
```
0x00000000 - 0x00000FFF  Sector 0: SensorFlashBuffer metadata (4KB)
0x00001000 - 0x07FFFFFF  Sensor data region (~128MB)
```

**Proposed layout:**
```
0x00000000 - 0x00000FFF  Sector 0: SensorFlashBuffer metadata (4KB)
0x00001000 - 0x00001FFF  Sector 1: LogFlashBuffer metadata (4KB)
0x00002000 - 0x05FFFFFF  Sensor data region (~96MB, ~8M records, ~7.6 years at 30s)
0x06000000 - 0x07FFFFFF  Log data region (32MB)
```

This requires updating `SensorFlashBuffer::DATA_START_OFFSET` from `0x1000` to `0x2000` and adding a `DATA_END_OFFSET` constant (currently it uses the full flash size).

**Log region capacity:**
- 32MB / 128 bytes per record = 262,144 log entries
- At ~1 log/second average: ~3 days of continuous logging
- At normal operation (~1 log every few seconds): weeks of history

### 2. Log Record Format

Fixed-size 128-byte records for simplicity and alignment with flash page boundaries (2 records per 256-byte page, 32 records per 4KB sector).

```cpp
struct __attribute__((packed)) LogRecord {
    uint32_t timestamp;     // 4 bytes  - ms since boot (or unix time if RTC valid)
    uint8_t  level;         // 1 byte   - LogLevel enum value
    char     module[11];    // 11 bytes - module name, null-terminated
    char     message[108];  // 108 bytes - message text, null-terminated
    uint16_t sequence;      // 2 bytes  - rolling sequence number
    uint16_t crc16;         // 2 bytes  - CRC16-CCITT integrity check
};                          // Total: 128 bytes

static_assert(sizeof(LogRecord) == 128, "LogRecord must be 128 bytes");
```

### 3. LogFlashBuffer

A new class modeled on `SensorFlashBuffer`, but simplified for log entries.

```cpp
class LogFlashBuffer {
public:
    // Flash layout constants
    static constexpr uint32_t METADATA_OFFSET = 0x00001000;  // Sector 1
    static constexpr uint32_t DATA_START = 0x06000000;
    static constexpr uint32_t DATA_END = 0x08000000;  // 128MB boundary
    static constexpr uint32_t DATA_SIZE = DATA_END - DATA_START;  // 32MB
    static constexpr uint32_t MAX_RECORDS = DATA_SIZE / sizeof(LogRecord);

    explicit LogFlashBuffer(ExternalFlash& flash);

    bool init();
    bool writeLog(LogLevel level, const char* module, const char* message);
    bool readRecord(uint32_t index, LogRecord& record);
    uint32_t getRecordCount() const;
    bool flush();
    bool reset();

private:
    ExternalFlash& flash_;
    LogFlashMetadata metadata_;
    // RAM write buffer: accumulate records before flushing a full page
    LogRecord page_buffer_[2];  // 256 bytes = 1 flash page
    uint8_t page_buffer_count_;
    bool initialized_;
    Logger logger_;
};
```

**Key behaviors:**
- Circular buffer: oldest logs overwritten when full
- Page-buffered writes: accumulate 2 records (256 bytes = 1 page) in RAM before writing to flash, reducing flash operations
- Sector erase handled automatically on wrap-around (erase next sector before writing to it)
- CRC16 per record for integrity validation on read
- Metadata persisted to its own sector with flush-on-demand

### 4. Logger Integration

Add an optional flash sink to the existing `Logger` class:

```cpp
class Logger {
    // ... existing interface unchanged ...

    // New: flash log sink
    static void setFlashSink(LogFlashBuffer* buffer);
    static void setFlashLogLevel(LogLevel level);  // can differ from console level

private:
    static LogFlashBuffer* flash_sink_;
    static LogLevel flash_level_;  // default: LogLevel::Warn (save flash for important stuff)
};
```

In the `log()` method, after the existing printf output, also write to the flash sink if configured:

```cpp
void log(LogLevel level, const char* prefix, const char* fmt, va_list args) const {
    // ... existing printf logic ...

    // Write to flash if configured and level meets threshold
    if (flash_sink_ && static_cast<uint8_t>(flash_level_) >= static_cast<uint8_t>(level)) {
        char msg_buf[108];
        vsnprintf(msg_buf, sizeof(msg_buf), fmt, args);
        flash_sink_->writeLog(level, module_name_, msg_buf);
    }
}
```

**Configuration:**
- `flash_level_` defaults to `LogLevel::Info` -- captures Info/Warn/Error to flash
- Console level remains independently configurable
- Flash logging can be set to `LogLevel::Debug` for deep debugging sessions
- A `va_list` cannot be consumed twice (undefined behavior), so we need to `va_copy` before the flash write

### 5. Log Retrieval

#### Option A: USB Serial Dump (simple, implement first)

Add a simple command on USB CDC serial to dump stored logs:

```cpp
// In main loop or a dedicated task:
// When user sends "DUMP_LOGS\n" over USB serial:
void handleLogDumpCommand() {
    LogRecord record;
    for (uint32_t i = 0; i < logBuffer.getRecordCount(); i++) {
        if (logBuffer.readRecord(i, record)) {
            printf("[%lu] %s [%s]: %s\n",
                   record.timestamp,
                   levelToString(record.level),
                   record.module,
                   record.message);
        }
    }
}
```

This requires no USB stack changes. The user can capture output with a terminal emulator (e.g., `screen /dev/ttyACM0 115200 > logs.txt`).

#### Option B: USB Mass Storage (user's preferred approach)

Present the log region as a read-only USB drive containing a `logs.txt` file.

**Architecture:**
1. Replace `pico_stdio_usb` with a custom composite CDC+MSC TinyUSB device
2. CDC interface preserves printf/serial functionality
3. MSC interface presents a read-only FAT12 filesystem
4. The FAT12 structures (boot sector, FAT table, root directory) are generated in RAM (~1KB)
5. The single file `LOGS.TXT` maps to the log data region on external flash
6. `tud_msc_read10_cb` serves FAT metadata from RAM and log data from flash (formatted as text on-the-fly)

**Mode activation:**
- **Always available** (read-only, no conflict with logging): MSC is always mounted. Firmware continues writing logs. The host sees a point-in-time snapshot.
- **OR button-activated** (simpler, avoids concurrent access): Hold button on boot to enter USB MSC mode. No logging/sensor activity. Clean read of flash.

**Recommendation:** Start with button-activated mode for simplicity. The "always available" mode requires careful handling of concurrent flash access (firmware writing while host reads).

**TinyUSB implementation requires:**

| File | Purpose |
|------|---------|
| `tusb_config.h` | Enable CFG_TUD_CDC=1, CFG_TUD_MSC=1 |
| `usb_descriptors.c` | Composite CDC+MSC device/config/interface descriptors |
| `msc_callbacks.c` | Implement tud_msc_read10_cb, tud_msc_capacity_cb, etc. |
| `fat12.c` | Generate FAT12 boot sector, FAT, root directory in RAM |

**CMake changes:**
- Replace `pico_enable_stdio_usb(TARGET 1)` with direct `tinyusb_device` linkage
- Add custom USB source files

### 6. On-the-Fly Text Formatting (for USB MSC)

When the USB host reads `LOGS.TXT`, the MSC read callback translates binary `LogRecord` data into human-readable text lines:

```
[2026-02-10 14:23:45.123] INFO [MAIN]: LoRa module initialized
[2026-02-10 14:23:45.250] WARN [LORA]: Signal strength low, RSSI=-95
[+1234ms] ERR [FLASH]: Write verify failed at sector 42
```

Each line is ~80-160 bytes. The MSC callback computes which log records correspond to the requested LBA range and formats them into the read buffer. The reported file size is computed from record count * estimated average line length.

This is the most complex part of the MSC approach. An alternative is to serve the raw binary records and provide a host-side Python script to decode them.

## Recommended Implementation Phases

### Phase 1: Log Storage Core
- Define `LogRecord` and `LogFlashMetadata` structures
- Implement `LogFlashBuffer` (circular buffer on external flash)
- Update external flash partitioning constants in `SensorFlashBuffer`
- Unit test with existing test framework

### Phase 2: Logger Integration
- Add flash sink to `Logger` class
- Wire up in `main.cpp` initialization (after ExternalFlash init)
- Configure appropriate flash log level
- Handle `va_copy` for dual output (printf + flash)

### Phase 3: USB Serial Dump
- Add simple command handler for `DUMP_LOGS` over USB CDC
- Test end-to-end: boot -> generate logs -> dump via serial

### Phase 4: USB Mass Storage
- Create custom `tusb_config.h` for composite CDC+MSC
- Implement USB descriptors
- Implement MSC callbacks backed by external flash log region
- Implement FAT12 generation (or use a minimal library)
- Add button-activated mode switch
- Test with Windows/Mac/Linux hosts

## Risks and Mitigations

| Risk | Impact | Mitigation |
|------|--------|------------|
| Flash write during interrupt-sensitive LoRa operations | Missed LoRa interrupts | Buffer log writes in RAM, flush during idle periods |
| SPI bus contention (flash + LoRa share SPI1) | Communication errors | SPI bus is already arbitrated; flash operations wait for LoRa |
| USB MSC + CDC composite complexity | Development effort | Phase 4 is optional; Phase 3 (serial dump) provides 80% of the value |
| Log volume fills flash quickly in debug mode | Lost old logs | Configurable flash log level; default to Info (not Debug) |
| va_list double consumption | Undefined behavior | Use va_copy before flash write |

## Decision Points

1. **Flash region size for logs**: 32MB proposed. Could be 8MB (65K entries) or 64MB (500K entries) depending on needs. Adjustable at compile time.
2. **Flash log level default**: Info (captures Info/Warn/Error) vs Warn (only Warn/Error). Info is more useful for debugging but fills flash faster.
3. **USB MSC mode**: Always-on vs button-activated. Button-activated is simpler and safer.
4. **Log text format for MSC**: On-the-fly text conversion vs raw binary + host decode script. Text is more user-friendly but more complex firmware-side.
