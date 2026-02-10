/**
 * @file usb_descriptors.c
 * @brief USB descriptors for composite CDC + MSC device
 *
 * Provides device, configuration, string, and HID descriptors for a
 * composite USB device with both CDC (serial) and MSC (mass storage).
 */

#include "tusb.h"
#include "pico/unique_id.h"

// USB VID/PID - using Raspberry Pi test PIDs
#define USB_VID   0x2E8A  // Raspberry Pi
#define USB_PID   0x000A  // Pico SDK CDC+MSC

// String descriptor indices
enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
    STRID_CDC,
    STRID_MSC,
};

// Interface numbers
enum {
    ITF_NUM_CDC = 0,
    ITF_NUM_CDC_DATA,
    ITF_NUM_MSC,
    ITF_NUM_TOTAL
};

// Endpoint numbers
#define EPNUM_CDC_NOTIF   0x81
#define EPNUM_CDC_OUT     0x02
#define EPNUM_CDC_IN      0x82
#define EPNUM_MSC_OUT     0x03
#define EPNUM_MSC_IN      0x83

//--------------------------------------------------------------------
// Device Descriptor
//--------------------------------------------------------------------
static const tusb_desc_device_t desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0210,  // USB 2.1 for BOS
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    .bcdDevice          = 0x0100,
    .iManufacturer      = STRID_MANUFACTURER,
    .iProduct           = STRID_PRODUCT,
    .iSerialNumber      = STRID_SERIAL,
    .bNumConfigurations = 0x01
};

uint8_t const *tud_descriptor_device_cb(void)
{
    return (uint8_t const *)&desc_device;
}

//--------------------------------------------------------------------
// Configuration Descriptor
//--------------------------------------------------------------------
#define CONFIG_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_MSC_DESC_LEN)

static const uint8_t desc_configuration[] = {
    // Config descriptor
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN,
                          0x00, 100),

    // CDC: Interface Association + CDC Control + CDC Data
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, STRID_CDC, EPNUM_CDC_NOTIF,
                       8, EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),

    // MSC: Bulk-Only Transport
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, STRID_MSC, EPNUM_MSC_OUT,
                       EPNUM_MSC_IN, 64),
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return desc_configuration;
}

//--------------------------------------------------------------------
// String Descriptors
//--------------------------------------------------------------------
static const char *string_desc_arr[] = {
    [STRID_LANGID]       = (const char[]){0x09, 0x04},  // English (US)
    [STRID_MANUFACTURER] = "Bramble",
    [STRID_PRODUCT]      = "Bramble Log Storage",
    [STRID_SERIAL]       = NULL,  // Filled dynamically from board ID
    [STRID_CDC]          = "Bramble Serial",
    [STRID_MSC]          = "Bramble Log Drive",
};

// Buffer for string descriptor response
static uint16_t _desc_str[33];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;

    uint8_t chr_count;

    if (index == 0) {
        memcpy(&_desc_str[1], string_desc_arr[0], 2);
        chr_count = 1;
    } else {
        const char *str;
        char serial_str[17];  // 8 bytes hex = 16 chars + null

        if (index == STRID_SERIAL) {
            // Generate serial from unique board ID
            pico_unique_board_id_t board_id;
            pico_get_unique_board_id(&board_id);
            for (int i = 0; i < 8; i++) {
                sprintf(&serial_str[i * 2], "%02X", board_id.id[i]);
            }
            str = serial_str;
        } else if (index < sizeof(string_desc_arr) / sizeof(string_desc_arr[0])) {
            str = string_desc_arr[index];
        } else {
            return NULL;
        }

        if (!str) return NULL;

        chr_count = (uint8_t)strlen(str);
        if (chr_count > 31) chr_count = 31;

        for (uint8_t i = 0; i < chr_count; i++) {
            _desc_str[1 + i] = str[i];
        }
    }

    // First word is length (including header) and string descriptor type
    _desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));

    return _desc_str;
}
