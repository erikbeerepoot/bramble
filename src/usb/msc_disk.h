#pragma once

#include <stdint.h>

// Forward declarations
class LogFlashBuffer;
class ExternalFlash;

/**
 * @brief Initialize the MSC disk backend
 *
 * Sets up the FAT12 filesystem in RAM that presents log records as LOGS.TXT.
 * Must be called before USB enumeration.
 *
 * @param log_buffer The log flash buffer to read from
 * @param flash The external flash for direct reads
 */
void msc_disk_init(LogFlashBuffer *log_buffer, ExternalFlash *flash);
