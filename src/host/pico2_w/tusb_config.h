#ifndef YAJIR_TUSB_CONFIG_H
#define YAJIR_TUSB_CONFIG_H

#ifndef CFG_TUSB_MCU
#error CFG_TUSB_MCU must be defined by the Pico SDK
#endif

#define CFG_TUSB_DEBUG              0
#define CFG_TUD_ENABLED             1
#define CFG_TUD_MAX_SPEED           OPT_MODE_DEFAULT_SPEED
#define CFG_TUD_ENDPOINT0_SIZE      64

#define CFG_TUD_CDC                 1
#define CFG_TUD_MSC                 1
#define CFG_TUD_HID                 0
#define CFG_TUD_MIDI                0
#define CFG_TUD_VENDOR              0

#define CFG_TUD_CDC_RX_BUFSIZE      64
#define CFG_TUD_CDC_TX_BUFSIZE      64
#define CFG_TUD_CDC_EP_BUFSIZE      64
#define CFG_TUD_MSC_EP_BUFSIZE      512

#endif /* YAJIR_TUSB_CONFIG_H */
