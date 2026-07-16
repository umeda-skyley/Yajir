/* usb_descriptors.c - Yajir CDC serial + MSC composite device */
#include <stddef.h>
#include <string.h>

#include "pico/unique_id.h"
#include "tusb.h"

#define USB_VID 0xCAFE
#define USB_PID 0x4003
#define USB_BCD 0x0100

static tusb_desc_device_t const s_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USB_VID,
    .idProduct = USB_PID,
    .bcdDevice = USB_BCD,
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 3,
    .bNumConfigurations = 1
};

uint8_t const *tud_descriptor_device_cb(void)
{
    return (uint8_t const *)&s_device;
}

enum {
    ITF_CDC = 0,
    ITF_CDC_DATA,
    ITF_MSC,
    ITF_TOTAL
};

#define EP_CDC_NOTIFY 0x81
#define EP_CDC_OUT    0x02
#define EP_CDC_IN     0x82
#define EP_MSC_OUT    0x03
#define EP_MSC_IN     0x83
#define CONFIG_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_MSC_DESC_LEN)

static uint8_t const s_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_TOTAL, 0, CONFIG_LEN, 0x00, 100),
    TUD_CDC_DESCRIPTOR(ITF_CDC, 4, EP_CDC_NOTIFY, 8,
                       EP_CDC_OUT, EP_CDC_IN, 64),
    TUD_MSC_DESCRIPTOR(ITF_MSC, 5, EP_MSC_OUT, EP_MSC_IN, 64)
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return s_configuration;
}

static char const *const s_strings[] = {
    NULL,
    "Yajir",
    "Yajir Pico 2 W",
    NULL,
    "Yajir USB Serial",
    "YAJIR Script Drive"
};

static uint16_t s_string_desc[33];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    char serial[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
    char const *text;
    size_t count;
    size_t i;

    (void)langid;
    if (index == 0) {
        s_string_desc[1] = 0x0409;
        count = 1;
    } else {
        if (index >= sizeof(s_strings) / sizeof(s_strings[0])) return NULL;
        if (index == 3) {
            pico_get_unique_board_id_string(serial, sizeof(serial));
            text = serial;
        } else {
            text = s_strings[index];
        }
        if (!text) return NULL;
        count = strlen(text);
        if (count > 32) count = 32;
        for (i = 0; i < count; ++i)
            s_string_desc[i + 1] = (uint8_t)text[i];
    }

    s_string_desc[0] = (uint16_t)((TUSB_DESC_STRING << 8) |
                                  (2u * count + 2u));
    return s_string_desc;
}
