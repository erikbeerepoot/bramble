/**
 * @file msc_disk.cpp
 * @brief MSC callbacks presenting log flash data as a read-only FAT12 USB drive
 *
 * Layout:
 * - Block 0:        Boot sector
 * - Blocks 1-2:     FAT12 table
 * - Blocks 3-4:     Root directory (1 file entry: LOGS.TXT)
 * - Blocks 5+:      File data (log records formatted as text lines)
 *
 * All FAT12 metadata is generated in RAM. File data is generated on-the-fly
 * from log records in external flash.
 */

#include "tusb.h"
#include "msc_disk.h"
#include "../storage/log_flash_buffer.h"
#include "../storage/log_record.h"
#include "../hal/external_flash.h"

#include <cstdio>
#include <cstring>

// Disk geometry
static constexpr uint16_t DISK_BLOCK_SIZE = 512;
static constexpr uint16_t RESERVED_SECTORS = 1;   // Boot sector
static constexpr uint16_t FAT_SECTORS = 2;         // FAT12 table
static constexpr uint16_t ROOT_DIR_SECTORS = 2;    // Root directory (32 entries)
static constexpr uint16_t DATA_START_SECTOR = RESERVED_SECTORS + FAT_SECTORS + ROOT_DIR_SECTORS; // = 5
static constexpr uint16_t SECTORS_PER_CLUSTER = 1;

// Maximum formatted line: "[4294967295] ERR  [MODULENAME]: <108 chars message>\r\n"
// Worst case ~ 140 chars. Use 160 for safety.
static constexpr size_t MAX_LINE_LENGTH = 160;

// State
static LogFlashBuffer *g_log_buffer = nullptr;
static ExternalFlash *g_flash = nullptr;
static uint32_t g_file_size = 0;      // Total file size in bytes
static uint32_t g_total_blocks = 0;   // Total disk blocks

// Pre-computed FAT12 and boot sector
static uint8_t g_boot_sector[512];
static uint8_t g_fat[1024];           // 2 sectors of FAT12
static uint8_t g_root_dir[1024];      // 2 sectors of root directory

static const char *level_str(uint8_t level)
{
    switch (level) {
        case 1: return "ERR ";
        case 2: return "WARN";
        case 3: return "INFO";
        case 4: return "DBG ";
        default: return "??? ";
    }
}

/**
 * @brief Format a log record into a text line
 * @return Number of bytes written (excluding null terminator)
 */
static int format_log_line(const LogRecord &record, char *buf, size_t buf_size)
{
    return snprintf(buf, buf_size, "[%lu] %s [%.11s]: %.108s\r\n",
                    (unsigned long)record.timestamp,
                    level_str(record.level),
                    record.module,
                    record.message);
}

/**
 * @brief Estimate the total file size by formatting all records
 *
 * We compute this once at init time so the FAT and directory entry
 * report a consistent file size.
 */
static uint32_t compute_file_size()
{
    if (!g_log_buffer) return 0;

    uint32_t count = g_log_buffer->getStoredCount();
    if (count == 0) return 0;

    // Sample a few records to get average line length, then multiply
    // For accuracy with minimal flash reads, sample up to 8 records
    uint32_t oldest = g_log_buffer->getOldestIndex();
    uint32_t max_records = g_log_buffer->MAX_RECORDS;
    uint32_t total_len = 0;
    uint32_t sampled = 0;
    char line_buf[MAX_LINE_LENGTH];

    for (uint32_t i = 0; i < 8 && i < count; i++) {
        uint32_t idx = (oldest + i * (count / 8)) % max_records;
        LogRecord record;
        if (g_log_buffer->readRecord(idx, record)) {
            int len = format_log_line(record, line_buf, sizeof(line_buf));
            if (len > 0) {
                total_len += (uint32_t)len;
                sampled++;
            }
        }
    }

    if (sampled == 0) return 0;

    uint32_t avg_len = total_len / sampled;
    return avg_len * count;
}

static void build_boot_sector()
{
    memset(g_boot_sector, 0, sizeof(g_boot_sector));

    // BPB (BIOS Parameter Block)
    g_boot_sector[0] = 0xEB; g_boot_sector[1] = 0x3C; g_boot_sector[2] = 0x90; // Jump + NOP
    memcpy(&g_boot_sector[3], "BRAMBLE ", 8);  // OEM name

    // Bytes per sector
    g_boot_sector[11] = (DISK_BLOCK_SIZE & 0xFF);
    g_boot_sector[12] = (DISK_BLOCK_SIZE >> 8);

    g_boot_sector[13] = SECTORS_PER_CLUSTER;   // Sectors per cluster
    g_boot_sector[14] = RESERVED_SECTORS;       // Reserved sectors (little-endian)
    g_boot_sector[15] = 0;
    g_boot_sector[16] = 1;                      // Number of FATs
    g_boot_sector[17] = 32;                     // Root dir entries (little-endian)
    g_boot_sector[18] = 0;

    // Total sectors (16-bit)
    uint16_t total_sectors = (uint16_t)g_total_blocks;
    g_boot_sector[19] = (total_sectors & 0xFF);
    g_boot_sector[20] = (total_sectors >> 8);

    g_boot_sector[21] = 0xF8;                  // Media type (fixed disk)

    // Sectors per FAT
    g_boot_sector[22] = FAT_SECTORS;
    g_boot_sector[23] = 0;

    g_boot_sector[24] = 1; g_boot_sector[25] = 0;  // Sectors per track
    g_boot_sector[26] = 1; g_boot_sector[27] = 0;  // Number of heads
    g_boot_sector[38] = 0x29;                        // Extended boot signature
    g_boot_sector[39] = 0x12; g_boot_sector[40] = 0x34;
    g_boot_sector[41] = 0x56; g_boot_sector[42] = 0x78;  // Volume serial

    memcpy(&g_boot_sector[43], "BRAMBLE LOG", 11);  // Volume label
    memcpy(&g_boot_sector[54], "FAT12   ", 8);      // FS type

    // Boot signature
    g_boot_sector[510] = 0x55;
    g_boot_sector[511] = 0xAA;
}

static void build_fat()
{
    memset(g_fat, 0, sizeof(g_fat));

    // FAT12 entries: first 2 entries are reserved
    g_fat[0] = 0xF8;  // Media byte
    g_fat[1] = 0xFF;
    g_fat[2] = 0xFF;

    // Build cluster chain for the file
    // Data clusters start at cluster 2
    uint32_t data_sectors = (g_file_size + DISK_BLOCK_SIZE - 1) / DISK_BLOCK_SIZE;
    if (data_sectors == 0) data_sectors = 1;  // At least 1 cluster even for empty file

    // FAT12 packing: each pair of entries uses 3 bytes
    for (uint32_t cluster = 2; cluster < data_sectors + 2; cluster++) {
        uint16_t value;
        if (cluster == data_sectors + 1) {
            value = 0xFFF;  // End of chain
        } else {
            value = (uint16_t)(cluster + 1);  // Next cluster
        }

        // FAT12 encoding
        uint32_t fat_offset = cluster + (cluster / 2);
        if (fat_offset + 1 >= sizeof(g_fat)) break;

        if (cluster & 1) {
            // Odd cluster
            g_fat[fat_offset] = (g_fat[fat_offset] & 0x0F) | ((value & 0x0F) << 4);
            g_fat[fat_offset + 1] = (uint8_t)(value >> 4);
        } else {
            // Even cluster
            g_fat[fat_offset] = (uint8_t)(value & 0xFF);
            g_fat[fat_offset + 1] = (g_fat[fat_offset + 1] & 0xF0) | ((value >> 8) & 0x0F);
        }
    }
}

static void build_root_dir()
{
    memset(g_root_dir, 0, sizeof(g_root_dir));

    // Volume label entry
    memcpy(&g_root_dir[0], "BRAMBLE LOG", 11);
    g_root_dir[11] = 0x08;  // Attribute: volume label

    // File entry: LOGS.TXT
    uint8_t *entry = &g_root_dir[32];
    memcpy(entry, "LOGS    TXT", 11);  // 8.3 filename
    entry[11] = 0x01;                   // Attribute: read-only
    entry[26] = 2;                      // Start cluster (little-endian)
    entry[27] = 0;

    // File size (little-endian, 32-bit)
    entry[28] = (g_file_size >>  0) & 0xFF;
    entry[29] = (g_file_size >>  8) & 0xFF;
    entry[30] = (g_file_size >> 16) & 0xFF;
    entry[31] = (g_file_size >> 24) & 0xFF;
}

void msc_disk_init(LogFlashBuffer *log_buffer, ExternalFlash *flash)
{
    g_log_buffer = log_buffer;
    g_flash = flash;

    g_file_size = compute_file_size();

    // Total data sectors for file content
    uint32_t file_sectors = (g_file_size + DISK_BLOCK_SIZE - 1) / DISK_BLOCK_SIZE;
    if (file_sectors == 0) file_sectors = 1;

    g_total_blocks = DATA_START_SECTOR + file_sectors;

    // Cap at 65535 (FAT12 limit for 16-bit total sectors)
    if (g_total_blocks > 65535) g_total_blocks = 65535;

    build_boot_sector();
    build_fat();
    build_root_dir();
}

//--------------------------------------------------------------------
// TinyUSB MSC Callbacks
//--------------------------------------------------------------------

extern "C" {

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8],
                         uint8_t product_id[16], uint8_t product_rev[4])
{
    (void)lun;
    memcpy(vendor_id,   "Bramble ", 8);
    memcpy(product_id,  "Log Storage     ", 16);
    memcpy(product_rev, "1.0 ", 4);
}

bool tud_msc_test_unit_ready_cb(uint8_t lun)
{
    (void)lun;
    return g_log_buffer != nullptr;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size)
{
    (void)lun;
    *block_count = g_total_blocks;
    *block_size = DISK_BLOCK_SIZE;
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition,
                            bool start, bool load_eject)
{
    (void)lun; (void)power_condition; (void)start; (void)load_eject;
    return true;
}

/**
 * @brief Read a block from our virtual FAT12 disk
 *
 * Blocks 0-4 are FAT12 metadata (served from RAM).
 * Blocks 5+ are file data (log records formatted on-the-fly).
 */
int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                           void *buffer, uint32_t bufsize)
{
    (void)lun;
    (void)offset;

    uint8_t *buf = (uint8_t *)buffer;

    if (lba >= g_total_blocks) {
        memset(buf, 0, bufsize);
        return (int32_t)bufsize;
    }

    // Metadata blocks
    if (lba == 0) {
        memcpy(buf, g_boot_sector, bufsize < 512 ? bufsize : 512);
        return (int32_t)bufsize;
    }

    if (lba >= RESERVED_SECTORS && lba < RESERVED_SECTORS + FAT_SECTORS) {
        uint32_t fat_offset = (lba - RESERVED_SECTORS) * DISK_BLOCK_SIZE;
        uint32_t copy_len = bufsize < 512 ? bufsize : 512;
        if (fat_offset + copy_len <= sizeof(g_fat)) {
            memcpy(buf, &g_fat[fat_offset], copy_len);
        } else {
            memset(buf, 0, copy_len);
        }
        return (int32_t)bufsize;
    }

    if (lba >= RESERVED_SECTORS + FAT_SECTORS &&
        lba < (uint32_t)DATA_START_SECTOR) {
        uint32_t dir_offset = (lba - RESERVED_SECTORS - FAT_SECTORS) * DISK_BLOCK_SIZE;
        uint32_t copy_len = bufsize < 512 ? bufsize : 512;
        if (dir_offset + copy_len <= sizeof(g_root_dir)) {
            memcpy(buf, &g_root_dir[dir_offset], copy_len);
        } else {
            memset(buf, 0, copy_len);
        }
        return (int32_t)bufsize;
    }

    // File data blocks - generate text from log records on-the-fly
    // We need to figure out which log records correspond to this file offset
    uint32_t file_offset = (lba - DATA_START_SECTOR) * DISK_BLOCK_SIZE;

    if (!g_log_buffer || g_log_buffer->getStoredCount() == 0) {
        memset(buf, 0, bufsize);
        return (int32_t)bufsize;
    }

    // Walk through log records to find the ones that map to this block.
    // This is O(n) per block read which isn't ideal, but log retrieval is
    // an infrequent offline operation and the USB speed is the bottleneck anyway.
    uint32_t count = g_log_buffer->getStoredCount();
    uint32_t oldest = g_log_buffer->getOldestIndex();
    uint32_t max_records = g_log_buffer->MAX_RECORDS;

    uint32_t current_offset = 0;
    uint32_t bytes_written = 0;
    char line_buf[MAX_LINE_LENGTH];

    memset(buf, 0, bufsize);

    for (uint32_t i = 0; i < count && bytes_written < bufsize; i++) {
        uint32_t idx = (oldest + i) % max_records;
        LogRecord record;

        if (!g_log_buffer->readRecord(idx, record)) {
            continue;  // Skip invalid records
        }

        int line_len = format_log_line(record, line_buf, sizeof(line_buf));
        if (line_len <= 0) continue;

        uint32_t line_end = current_offset + (uint32_t)line_len;

        // Check if this line overlaps with the requested block
        if (line_end > file_offset && current_offset < file_offset + bufsize) {
            // Calculate overlap
            uint32_t src_start = 0;
            uint32_t dst_start = 0;

            if (current_offset < file_offset) {
                src_start = file_offset - current_offset;
            } else {
                dst_start = current_offset - file_offset;
            }

            uint32_t copy_len = (uint32_t)line_len - src_start;
            if (dst_start + copy_len > bufsize) {
                copy_len = bufsize - dst_start;
            }

            memcpy(buf + dst_start, line_buf + src_start, copy_len);
            bytes_written = dst_start + copy_len;
        }

        current_offset = line_end;

        // If we've passed the block entirely, stop
        if (current_offset >= file_offset + bufsize) break;
    }

    return (int32_t)bufsize;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                            uint8_t *buffer, uint32_t bufsize)
{
    (void)lun; (void)lba; (void)offset; (void)buffer;
    // Read-only drive
    return -1;
}

int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16],
                          void *buffer, uint16_t bufsize)
{
    (void)buffer; (void)bufsize;

    int32_t resplen = 0;

    switch (scsi_cmd[0]) {
        case SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL:
            // Host is trying to lock/unlock - always allow
            resplen = 0;
            break;

        default:
            // Set Sense = Invalid Command Operation
            tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);
            resplen = -1;
            break;
    }

    return resplen;
}

bool tud_msc_is_writable_cb(uint8_t lun)
{
    (void)lun;
    return false;  // Read-only
}

}  // extern "C"
