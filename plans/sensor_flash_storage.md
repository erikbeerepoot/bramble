# Sensor Data Storage with USB Mass Storage Access

## Overview
Add external SPI flash storage to sensor nodes for local data logging, with USB mass storage capability for easy data retrieval.

## Goals
1. Store sensor readings locally on 1Gbit SPI flash (Strategy C: 4KB sector batching)
2. Enable USB mass storage mode to download data via drag-and-drop
3. Maintain efficient flash usage (99.85%) and minimize wear

## Storage Capacity Analysis

### Data Format
- **Per reading:** 10 bytes (timestamp + sensor type + length + temp + humidity)
- **Readings per minute:** 1
- **Daily data:** 1,440 readings × 10 bytes = 14.4 KB/day
- **Annual data:** 5.3 MB/year

### Flash Specifications
- **Capacity:** 1 Gbit = 128 MB
- **Sector size:** 4 KB (erase granularity)
- **Page size:** 256 bytes (write granularity)
- **Erase cycles:** 100,000 per sector

### Strategy C: Sector-Based Storage
- **Readings per sector:** 409 (4090 bytes / 10 bytes per reading)
- **Efficiency:** 99.85% (4090/4096 bytes used)
- **Total sectors:** 32,768
- **Total capacity:** 13.4 million readings = ~25 years at 1/minute
- **Flash lifetime:** Each sector holds 6.8 hours, rotates every 25 years, 100K cycles = >2 million years

## Data Structures

### Core Reading Structure
```cpp
struct __attribute__((packed)) SensorReading {
    uint32_t timestamp;    // 4 bytes - Unix timestamp (seconds since epoch)
    uint8_t  sensor_type;  // 1 byte - SensorType enum
    uint8_t  data_length;  // 1 byte - Always 4 for temp+humidity
    int16_t  temperature;  // 2 bytes - Celsius × 100 (e.g., 2534 = 25.34°C)
    int16_t  humidity;     // 2 bytes - Percent × 100 (e.g., 6521 = 65.21%)
    // Total: 10 bytes
};
```

### Flash Sector Format
```cpp
struct __attribute__((packed)) FlashSector {
    uint16_t count;              // Number of readings in this sector (0-409)
    uint16_t checksum;           // CRC16 or simple checksum for integrity
    SensorReading readings[409]; // 10 bytes each = 4090 bytes
    // Total: 4094 bytes (fits in 4096-byte sector with 2 bytes padding)
};
```

### Flash Metadata (Sector 0)
```cpp
struct __attribute__((packed)) FlashMetadata {
    uint32_t magic;              // 0xBEEFDATA - verify initialized
    uint32_t total_readings;     // Total readings written (lifetime)
    uint32_t current_sector;     // Current sector being written
    uint16_t current_count;      // Count in current sector
    uint32_t oldest_sector;      // First sector with valid data (for circular buffer)
    uint32_t format_version;     // Schema version for future compatibility
    uint8_t  reserved[4064];     // Rest of 4KB sector reserved
};
```

## Architecture

### Dual-Mode Operation

#### Mode A: Sensor Mode (Default)
Normal operation with LoRa communication and flash backup:

**On Wake Cycle:**
1. Take sensor reading
2. Send to hub via LoRa (BEST_EFFORT)
3. Read current sector from flash
4. Append reading to sector buffer
5. Write updated sector back to flash
6. If sector full (409 readings), advance to next sector
7. Update metadata
8. Sleep

**Flash Acts As:**
- Backup when hub offline
- Long-term historical archive
- Disaster recovery

#### Mode B: USB Mass Storage Mode
Activated by holding button during power-on:

**Functionality:**
- SPI flash exposed as USB mass storage device
- Appears as removable drive on PC
- Binary data converted to CSV on-the-fly
- Read-only access (prevents corruption)
- No LoRa, no sensor readings - just USB

**User Workflow:**
1. Power off sensor node
2. Hold button + plug into USB
3. LED indicates USB mode
4. PC mounts drive, shows CSV file(s)
5. Drag file to PC to download
6. Unplug, reboot → back to sensor mode

### Mode Selection Logic
```cpp
void main() {
    stdio_init_all();

    // Check button state
    gpio_init(DOWNLOAD_BUTTON_PIN);
    gpio_set_dir(DOWNLOAD_BUTTON_PIN, GPIO_IN);
    gpio_pull_up(DOWNLOAD_BUTTON_PIN);
    sleep_ms(10); // Debounce

    if (!gpio_get(DOWNLOAD_BUTTON_PIN)) {
        // Button pressed (pulled low)
        enter_usb_mass_storage_mode();
        // Never returns - stays in USB mode until reboot
    } else {
        // Normal operation
        enter_sensor_mode();
    }
}
```

## Implementation Details

### Phase 1: Flash Storage Layer

**Hardware:**
- 1Gbit (128MB) SPI flash chip (e.g., W25Q128, GD25Q128)
- Connected via SPI (can share with LoRa on different CS pin)
- Additional CS pin required (e.g., GPIO10)

**Driver:**
- Use Pico SDK's W25Q flash driver as base
- Implement sector read/write/erase operations
- Add metadata management

**Key Functions:**
```cpp
// Initialize flash and load metadata
bool flash_storage_init();

// Append reading to current sector
bool flash_append_reading(const SensorReading* reading);

// Read specific sector
bool flash_read_sector(uint32_t sector_num, FlashSector* sector);

// Get metadata
const FlashMetadata* flash_get_metadata();

// Format flash (clear all data)
bool flash_format();
```

### Phase 2: Sensor Integration

**Modify Sensor Mode:**
```cpp
void sensor_mode_loop() {
    while (1) {
        // Take reading
        SensorReading reading;
        reading.timestamp = get_current_timestamp();
        reading.sensor_type = SENSOR_TEMPERATURE;
        reading.data_length = 4;
        reading.temperature = read_temperature() * 100;
        reading.humidity = read_humidity() * 100;

        // Try sending to hub
        bool sent = send_to_hub(&reading);

        // Always save to flash (backup)
        flash_append_reading(&reading);

        // Sleep until next reading
        sleep_until_next_reading();
    }
}
```

**Circular Buffer Handling:**
- When reaching last sector, wrap to sector 1 (sector 0 is metadata)
- Oldest data automatically overwritten
- Update metadata with oldest_sector pointer

### Phase 3: USB Mass Storage

**Using TinyUSB (included in Pico SDK):**

**MSC Device Callbacks:**
```cpp
// Called when PC reads sectors
int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba,
                          uint32_t offset, void* buffer, uint32_t bufsize) {
    // Convert logical block address to flash sector
    // Read from SPI flash
    // Convert binary to CSV if needed
    return bufsize;
}

// Called when PC writes (we'll make it read-only)
int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba,
                           uint32_t offset, uint8_t* buffer, uint32_t bufsize) {
    // Return error - read-only device
    return -1;
}

// Report device capacity
void tud_msc_capacity_cb(uint8_t lun, uint32_t* block_count, uint16_t* block_size) {
    *block_size = 512;  // Standard sector size
    *block_count = calculate_csv_size_in_blocks();
}
```

**CSV Conversion:**
Two options:

**Option A: Single CSV File**
```csv
Timestamp,DateTime,Type,Temperature_C,Humidity_Percent
1735689600,2025-01-01 00:00:00,TEMP_HUM,25.34,65.21
1735689660,2025-01-01 00:01:00,TEMP_HUM,25.35,65.19
...
```

**Option B: Daily CSV Files (via FAT32)**
More complex but better UX:
```
/2025-01-01.csv
/2025-01-02.csv
/2025-01-03.csv
...
```

**Recommendation:** Start with Option A (simpler), add Option B later if needed.

### Phase 4: Testing & Validation

**Unit Tests:**
- Flash sector write/read cycles
- Metadata persistence
- Circular buffer wrap-around
- Data integrity (checksums)

**Integration Tests:**
- Full sensor reading → flash → USB retrieval flow
- Power cycle during write (ensure no corruption)
- Flash full scenario (oldest data overwritten)
- USB enumeration on Windows/Mac/Linux

**Field Tests:**
- 24-hour continuous operation
- Sleep/wake cycles
- LoRa communication + flash writes
- Data retrieval via USB

## Pin Assignments

### Additional Pins Required
- **Flash CS:** GPIO10 (or any free GPIO)
- **Download Button:** GPIO11 (or any free GPIO with pull-up)
- **Status LED:** GPIO12 (optional, for mode indication)

### SPI Configuration
Can share SPI bus with LoRa module (different CS pins):
- **SPI Bus:** SPI1 (already in use for LoRa)
- **LoRa CS:** GPIO16 (existing)
- **Flash CS:** GPIO10 (new)

## Power Considerations

### Flash Power Consumption
- **Active read:** ~10mA
- **Active write:** ~15mA
- **Standby:** <1µA
- **Deep power-down:** <1µA

### Impact on Battery Life
- Write operation: ~10ms @ 15mA = 150µAs
- Per reading (every 60s): negligible impact
- Use flash deep power-down between writes

## Error Handling

### Flash Write Failures
- Retry write 3 times
- If still fails, mark sector as bad
- Skip to next sector
- Log error to metadata

### Data Corruption Detection
- CRC16 checksum per sector
- Validate on read
- Skip corrupted sectors during USB download
- Include error report in CSV

### Flash Full Scenario
- Automatically overwrite oldest data
- No user intervention needed
- Metadata tracks circular buffer position

## Future Enhancements

### Remote Data Download
Keep LoRa bulk transfer as backup:
- Special command: "DUMP_DATA start_time end_time"
- Node streams sectors over LoRa
- Hub reassembles and stores
- Useful when physical access difficult

### Data Compression
- Delta encoding (store differences)
- Could reduce size by 50-70%
- Trade-off: more CPU/power during write

### Multiple Sensor Types
- Expand SensorReading for different sensors
- Soil moisture, light, etc.
- Variable data_length field supports this

### Wear Leveling
- Current design already provides excellent wear distribution
- Could add explicit bad block management
- Not critical given >2M year lifetime

## Bill of Materials

### Required Hardware
- **SPI Flash:** W25Q128 or GD25Q128 (128Mbit = 16MB)
  - Alternative: W25Q01 (1Gbit = 128MB) for full capacity
- **Button:** Tactile switch for download mode
- **LED:** (Optional) Status indicator
- **Resistors:** Pull-up for button, current limiting for LED

### Estimated Cost
- SPI Flash chip: $1-3
- Button: $0.10
- LED: $0.10
- Total BOM addition: ~$2-4

## Success Criteria

### Must Have
- ✅ Store sensor readings to flash reliably
- ✅ Survive sleep/wake cycles without data loss
- ✅ USB mode allows data download as CSV
- ✅ No data corruption after power cycles
- ✅ Simple user experience (hold button → plug in → drag file)

### Nice to Have
- Multiple CSV files (daily/weekly)
- Real-time flash usage statistics
- Remote download via LoRa
- Data compression

## Timeline Estimate

- **Phase 1 (Flash Storage):** 2-3 days
- **Phase 2 (Sensor Integration):** 1-2 days
- **Phase 3 (USB Mass Storage):** 2-3 days
- **Phase 4 (Testing):** 1-2 days
- **Total:** ~1-2 weeks for complete implementation

## References

- [Pico SDK USB Device Examples](https://github.com/raspberrypi/pico-examples/tree/master/usb/device)
- [TinyUSB MSC Device](https://github.com/hathach/tinyusb/tree/master/examples/device/msc_dual_lun)
- [W25Qxx Flash Datasheet](https://www.winbond.com/hq/product/code-storage-flash-memory/serial-nor-flash/)
- [USB Mass Storage Class Specification](https://www.usb.org/document-library/mass-storage-class-specification-overview)
