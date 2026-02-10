#pragma once

class ExternalFlash;
class LogFlashBuffer;
class NeoPixel;

/**
 * @brief Enter USB Mass Storage mode
 *
 * Initializes the log flash buffer and presents it as a read-only USB drive.
 * This function never returns - the device stays in MSC mode until unplugged.
 *
 * @param led NeoPixel for status indication
 */
void enter_usb_msc_mode(NeoPixel &led);
