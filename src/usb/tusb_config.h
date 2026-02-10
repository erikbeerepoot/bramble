#pragma once

// TinyUSB configuration for composite CDC + MSC device
// This replaces the default Pico SDK CDC-only configuration

#define CFG_TUSB_RHPORT0_MODE OPT_MODE_DEVICE

// Device class configuration
#define CFG_TUD_CDC 1
#define CFG_TUD_MSC 1
#define CFG_TUD_HID 0
#define CFG_TUD_MIDI 0
#define CFG_TUD_VENDOR 0

// CDC FIFO sizes
#define CFG_TUD_CDC_RX_BUFSIZE 256
#define CFG_TUD_CDC_TX_BUFSIZE 256

// MSC buffer size (must be at least 512 for a single disk block)
#define CFG_TUD_MSC_EP_BUFSIZE 512
