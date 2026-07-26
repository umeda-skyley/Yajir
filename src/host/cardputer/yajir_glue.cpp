/* yajir_glue.cpp - M5Stack Cardputer display and host bindings */
#include "yajir_build_config.h"
#include <Arduino.h>
#include <FS.h>
#include <SD.h>
#include <M5Cardputer.h>
#include <string.h>

extern "C" {
#include "script.h"
#include "host_diag.h"
}

#include "yajir_glue.h"
#include "yajir_sd.h"

enum {
    YAJIR_KEY_BACKSPACE = 8,
    YAJIR_KEY_TAB = 9,
    YAJIR_KEY_ENTER = 13,
    YAJIR_KEY_MOD_SHIFT = 1,
    YAJIR_KEY_MOD_CTRL = 2,
    YAJIR_KEY_MOD_ALT = 4,
    YAJIR_KEY_MOD_FN = 8,
    YAJIR_KEY_MOD_OPT = 16,
    YAJIR_KEY_TRACK_MAX = 8,
    YAJIR_IMAGE_SLOTS = 8,
    YAJIR_IMAGE_PATH_LEN = 64
};

enum yajir_image_type_t {
    YAJIR_IMAGE_UNKNOWN,
    YAJIR_IMAGE_JPEG,
    YAJIR_IMAGE_PNG,
    YAJIR_IMAGE_BMP,
    YAJIR_IMAGE_QOI
};

struct yajir_image_entry_t {
    int32_t id;
    int32_t width;
    int32_t height;
    char path[YAJIR_IMAGE_PATH_LEN];
};

static portMUX_TYPE s_yajir_mux = portMUX_INITIALIZER_UNLOCKED;
static Point2D_t s_previous_keys[YAJIR_KEY_TRACK_MAX];
static size_t s_previous_key_count;
static int32_t s_tft_text_size = 2;
static uint32_t s_tft_text_color = TFT_WHITE;
static uint32_t s_tft_text_background = TFT_BLACK;
static int s_tft_text_background_set;
static M5Canvas s_frame(&M5Cardputer.Display);
static int s_frame_active;
static yajir_image_entry_t s_images[YAJIR_IMAGE_SLOTS];
static int32_t s_next_image_id = 1;

extern "C" void yajir_critical_enter(void)
{
    portENTER_CRITICAL(&s_yajir_mux);
}

extern "C" void yajir_critical_exit(void)
{
    portEXIT_CRITICAL(&s_yajir_mux);
}

extern "C" void yajir_putc(char c)
{
    Serial.write((uint8_t)c);
    if (c != '\r') M5Cardputer.Display.write((uint8_t)c);
}

extern "C" void yajir_puts(const char *s)
{
    if (!s) return;
    while (*s) yajir_putc(*s++);
}

extern "C" int32_t get_tick(void)
{
    return (int32_t)millis();
}

static void reg_in(const char *name, script_in_fn fn)
{
    script_register_in(name, fn, SCRIPT_T_INT);
    host_diag_note(name);
}

static void reg_out(const char *name, script_out_fn fn)
{
    script_register_out(name, fn);
    host_diag_note(name);
}

static void reg_inout(const char *name, script_in_fn get_fn,
                      script_out_fn set_fn)
{
    script_register_inout(name, get_fn, set_fn, SCRIPT_T_INT);
    host_diag_note(name);
}

static void reg_handler(const char *name)
{
    script_register_handler(name);
    host_diag_note(name);
}

static void reg_const(const char *name, int32_t value)
{
    script_register_const(name, value);
    host_diag_note(name);
}

static bool was_pressed(const Point2D_t& point)
{
    for (size_t i = 0; i < s_previous_key_count; ++i) {
        if (s_previous_keys[i] == point) return true;
    }
    return false;
}

static int32_t keyboard_modifiers(const Keyboard_Class::KeysState& state)
{
    int32_t modifiers = 0;

    if (state.shift) modifiers |= YAJIR_KEY_MOD_SHIFT;
    if (state.ctrl) modifiers |= YAJIR_KEY_MOD_CTRL;
    if (state.alt) modifiers |= YAJIR_KEY_MOD_ALT;
    if (state.fn) modifiers |= YAJIR_KEY_MOD_FN;
    if (state.opt) modifiers |= YAJIR_KEY_MOD_OPT;
    return modifiers;
}

static int32_t key_character(const Point2D_t& point,
                             const Keyboard_Class::KeysState& state)
{
    KeyValue_t value = M5Cardputer.Keyboard.getKeyValue(point);
    uint8_t raw = (uint8_t)value.value_first;

    if (state.ctrl && (raw == 'c' || raw == 'C')) return 0x03;

    switch (raw) {
        case KEY_TAB:
            return YAJIR_KEY_TAB;
        case KEY_BACKSPACE:
            return YAJIR_KEY_BACKSPACE;
        case KEY_ENTER:
            return YAJIR_KEY_ENTER;
        case KEY_FN:
        case KEY_OPT:
        case KEY_LEFT_CTRL:
        case KEY_LEFT_SHIFT:
        case KEY_LEFT_ALT:
            return 0;
        default:
            return (uint8_t)((state.ctrl || state.shift ||
                              M5Cardputer.Keyboard.capslocked())
                                 ? value.value_second
                                 : value.value_first);
    }
}

static void post_keyboard_event(int32_t character, int32_t modifiers)
{
    char text = (char)character;
    script_arg_t args[3];

    args[0] = SCRIPT_ARG_INT(character);
    args[1] = SCRIPT_ARG_INT(modifiers);
    args[2] = SCRIPT_ARG_STR(&text, 1);
    script_post_msg_v("KEYBOARD", 3, args);
}

static void th_speaker(int argc, const script_value_t *argv)
{
    const char *command;

    if (argc < 1 || !script_val_is_str(argv[0])) {
        script_set_result(-2);
        return;
    }
    command = script_resolve_str(argv[0]);

    if (!strcmp(command, "tone")) {
        int32_t frequency;
        int32_t duration;

        if (argc < 2 || script_val_is_str(argv[1])) {
            script_set_result(-2);
            return;
        }
        frequency = argv[1].i;
        duration = argc > 2 ? argv[2].i : 0;
        if (frequency <= 0) {
            M5Cardputer.Speaker.stop();
            script_set_result(0);
            return;
        }
        if (frequency < 20) frequency = 20;
        if (frequency > 20000) frequency = 20000;
        M5Cardputer.Speaker.tone(
            (float)frequency,
            duration > 0 ? (uint32_t)duration : UINT32_MAX,
            0, true);
        script_set_result(frequency);
        return;
    }
    if (!strcmp(command, "stop")) {
        M5Cardputer.Speaker.stop();
        script_set_result(0);
        return;
    }
    if (!strcmp(command, "setVolume")) {
        int32_t volume;

        if (argc < 2 || script_val_is_str(argv[1])) {
            script_set_result(-2);
            return;
        }
        volume = argv[1].i;
        if (volume < 0) volume = 0;
        if (volume > 255) volume = 255;
        M5Cardputer.Speaker.setVolume((uint8_t)volume);
        script_set_result(volume);
        return;
    }
    if (!strcmp(command, "getVolume")) {
        script_set_result(M5Cardputer.Speaker.getVolume());
        return;
    }
    if (!strcmp(command, "isPlaying")) {
        script_set_result(M5Cardputer.Speaker.isPlaying() ? 1 : 0);
        return;
    }

    script_set_result(-1);
}

static int display_bad_args(void)
{
    script_set_result(-2);
    return -2;
}

static LovyanGFX *display_target(void)
{
    return s_frame_active ?
        static_cast<LovyanGFX *>(&s_frame) :
        static_cast<LovyanGFX *>(&M5Cardputer.Display);
}

static int display_begin_frame(uint32_t color)
{
    if (!s_frame.getBuffer()) {
        s_frame.setPsram(false);
        s_frame.setColorDepth(16);
        if (!s_frame.createSprite(
                M5Cardputer.Display.width(), M5Cardputer.Display.height()))
            return -5;
    }

    s_frame_active = 1;
    s_frame.setTextSize((uint8_t)s_tft_text_size);
    if (s_tft_text_background_set)
        s_frame.setTextColor(s_tft_text_color, s_tft_text_background);
    else
        s_frame.setTextColor(s_tft_text_color);
    s_frame.fillScreen(color);
    s_frame.setCursor(0, 0);
    return 0;
}

static int display_end_frame(void)
{
    if (!s_frame_active || !s_frame.getBuffer()) return -3;
    s_frame.pushSprite(0, 0);
    s_frame_active = 0;
    return 0;
}

static yajir_image_type_t image_type_from_path(const char *path)
{
    const char *extension = strrchr(path, '.');

    if (!extension) return YAJIR_IMAGE_UNKNOWN;
    if (!strcasecmp(extension, ".jpg") || !strcasecmp(extension, ".jpeg"))
        return YAJIR_IMAGE_JPEG;
    if (!strcasecmp(extension, ".png")) return YAJIR_IMAGE_PNG;
    if (!strcasecmp(extension, ".bmp")) return YAJIR_IMAGE_BMP;
    if (!strcasecmp(extension, ".qoi")) return YAJIR_IMAGE_QOI;
    return YAJIR_IMAGE_UNKNOWN;
}

static int image_normalize_path(const char *name, char *path, size_t size)
{
    int written;

    if (!name || !name[0] || strstr(name, "..") || strchr(name, '\\') ||
        strchr(name, ':') || strchr(name, ','))
        return -2;
    written = snprintf(path, size, name[0] == '/' ? "%s" : "/%s", name);
    if (written < 0 || (size_t)written >= size) return -2;
    return image_type_from_path(path) == YAJIR_IMAGE_UNKNOWN ? -3 : 0;
}

static bool image_read(File& file, uint8_t *data, size_t size)
{
    return file.read(data, size) == size;
}

static uint32_t image_u32_be(const uint8_t *data)
{
    return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
           ((uint32_t)data[2] << 8) | data[3];
}

static uint32_t image_u32_le(const uint8_t *data)
{
    return ((uint32_t)data[3] << 24) | ((uint32_t)data[2] << 16) |
           ((uint32_t)data[1] << 8) | data[0];
}

static bool image_jpeg_dimensions(File& file, int32_t *width, int32_t *height)
{
    uint8_t data[9];

    if (!image_read(file, data, 2) || data[0] != 0xff || data[1] != 0xd8)
        return false;

    while (file.available()) {
        int marker;
        uint16_t length;

        do {
            marker = file.read();
        } while (marker >= 0 && marker != 0xff);
        if (marker < 0) break;
        do {
            marker = file.read();
        } while (marker == 0xff);
        if (marker < 0 || marker == 0xd9 || marker == 0xda) break;
        if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd8)) continue;
        if (!image_read(file, data, 2)) break;
        length = ((uint16_t)data[0] << 8) | data[1];
        if (length < 2) break;

        if (marker == 0xc0 || marker == 0xc1 || marker == 0xc2 ||
            marker == 0xc3 || marker == 0xc5 || marker == 0xc6 ||
            marker == 0xc7 || marker == 0xc9 || marker == 0xca ||
            marker == 0xcb || marker == 0xcd || marker == 0xce ||
            marker == 0xcf) {
            if (length < 7 || !image_read(file, data, 5)) break;
            *height = ((int32_t)data[1] << 8) | data[2];
            *width = ((int32_t)data[3] << 8) | data[4];
            return *width > 0 && *height > 0;
        }
        if (!file.seek(file.position() + length - 2)) break;
    }
    return false;
}

static bool image_dimensions(const char *path, yajir_image_type_t type,
                             int32_t *width, int32_t *height)
{
    uint8_t data[26];
    File file;
    bool valid = false;

    file = SD.open(path, FILE_READ);
    if (!file || file.isDirectory()) {
        if (file) file.close();
        return false;
    }

    if (type == YAJIR_IMAGE_JPEG) {
        valid = image_jpeg_dimensions(file, width, height);
    } else if (type == YAJIR_IMAGE_PNG) {
        valid = image_read(file, data, 24) &&
                !memcmp(data, "\x89PNG\r\n\x1a\n", 8);
        if (valid) {
            *width = (int32_t)image_u32_be(data + 16);
            *height = (int32_t)image_u32_be(data + 20);
        }
    } else if (type == YAJIR_IMAGE_BMP) {
        valid = image_read(file, data, 26) && data[0] == 'B' && data[1] == 'M';
        if (valid) {
            *width = (int32_t)image_u32_le(data + 18);
            *height = (int32_t)image_u32_le(data + 22);
            if (*width < 0) *width = -*width;
            if (*height < 0) *height = -*height;
        }
    } else if (type == YAJIR_IMAGE_QOI) {
        valid = image_read(file, data, 12) && !memcmp(data, "qoif", 4);
        if (valid) {
            *width = (int32_t)image_u32_be(data + 4);
            *height = (int32_t)image_u32_be(data + 8);
        }
    }
    file.close();
    return valid && *width > 0 && *height > 0;
}

static int display_image(const char *name, int32_t x, int32_t y,
                         int32_t target_width, int32_t target_height,
                         int32_t source_width, int32_t source_height)
{
    char path[YAJIR_IMAGE_PATH_LEN];
    yajir_image_type_t type;
    float scale_x = 1.0f;
    float scale_y = 1.0f;
    File file;
    bool drawn = false;
    int status;

    status = image_normalize_path(name, path, sizeof(path));
    if (status != 0) return status;
    type = image_type_from_path(path);
    if (!yajir_sd_ready() && yajir_sd_init() != 0) return -3;
    if ((source_width <= 0 || source_height <= 0) &&
        !image_dimensions(path, type, &source_width, &source_height))
        return -3;

    if (target_width > 0 && target_height > 0) {
        scale_x = (float)target_width / source_width;
        scale_y = (float)target_height / source_height;
    } else if (target_width > 0) {
        scale_x = scale_y = (float)target_width / source_width;
    } else if (target_height > 0) {
        scale_x = scale_y = (float)target_height / source_height;
    }

    {
        int32_t drawn_width = (int32_t)(source_width * scale_x + 0.999f);
        int32_t drawn_height = (int32_t)(source_height * scale_y + 0.999f);
        LovyanGFX *target = display_target();

        if (x >= target->width() || y >= target->height() ||
            x + drawn_width <= 0 || y + drawn_height <= 0)
            return 0;
    }

    file = SD.open(path, FILE_READ);
    if (!file || file.isDirectory()) {
        if (file) file.close();
        return -3;
    }

    if (type == YAJIR_IMAGE_JPEG)
        drawn = display_target()->drawJpg(
            &file, x, y, 0, 0, 0, 0, scale_x, scale_y);
    else if (type == YAJIR_IMAGE_PNG)
        drawn = display_target()->drawPng(
            &file, x, y, 0, 0, 0, 0, scale_x, scale_y);
    else if (type == YAJIR_IMAGE_BMP)
        drawn = display_target()->drawBmp(
            &file, x, y, 0, 0, 0, 0, scale_x, scale_y);
    else if (type == YAJIR_IMAGE_QOI)
        drawn = display_target()->drawQoi(
            &file, x, y, 0, 0, 0, 0, scale_x, scale_y);
    file.close();
    return drawn ? 0 : -3;
}

static yajir_image_entry_t *image_find(int32_t id)
{
    for (size_t i = 0; i < YAJIR_IMAGE_SLOTS; ++i) {
        if (s_images[i].id == id) return &s_images[i];
    }
    return NULL;
}

static int32_t image_load(const char *name)
{
    yajir_image_entry_t *entry = NULL;
    yajir_image_type_t type;
    char path[YAJIR_IMAGE_PATH_LEN];
    int32_t width = 0;
    int32_t height = 0;
    int status;

    status = image_normalize_path(name, path, sizeof(path));
    if (status != 0) return status;
    if (!yajir_sd_ready() && yajir_sd_init() != 0) return -3;
    type = image_type_from_path(path);
    if (!image_dimensions(path, type, &width, &height)) return -3;

    for (size_t i = 0; i < YAJIR_IMAGE_SLOTS; ++i) {
        if (s_images[i].id == 0) {
            entry = &s_images[i];
            break;
        }
    }
    if (!entry) return -4;

    if (s_next_image_id <= 0) s_next_image_id = 1;
    entry->id = s_next_image_id++;
    entry->width = width;
    entry->height = height;
    snprintf(entry->path, sizeof(entry->path), "%s", path);
    return entry->id;
}

static int image_release(int32_t id)
{
    yajir_image_entry_t *entry = image_find(id);

    if (!entry) return -3;
    memset(entry, 0, sizeof(*entry));
    return 0;
}

static void th_display(int argc, const script_value_t *argv)
{
    const char *command;

    if (argc < 1 || !script_val_is_str(argv[0])) {
        display_bad_args();
        return;
    }
    command = script_resolve_str(argv[0]);

    if (!strcmp(command, "setTextSize")) {
        int32_t size;

        if (argc < 2 || script_val_is_str(argv[1])) {
            display_bad_args();
            return;
        }
        size = argv[1].i;
        if (size < 1) size = 1;
        if (size > 8) size = 8;
        s_tft_text_size = size;
        display_target()->setTextSize((uint8_t)size);
        script_set_result(size);
        return;
    }
    if (!strcmp(command, "getTextSize")) {
        script_set_result(s_tft_text_size);
        return;
    }
    if (!strcmp(command, "setTextColor")) {
        if (argc < 2 || script_val_is_str(argv[1])) {
            display_bad_args();
            return;
        }
        s_tft_text_color = (uint32_t)argv[1].i;
        s_tft_text_background_set =
            argc > 2 && !script_val_is_str(argv[2]);
        if (s_tft_text_background_set) {
            s_tft_text_background = (uint32_t)argv[2].i;
            display_target()->setTextColor(
                s_tft_text_color, s_tft_text_background);
        } else {
            display_target()->setTextColor(s_tft_text_color);
        }
        script_set_result(0);
        return;
    }
    if (!strcmp(command, "setCursor")) {
        if (argc < 3) {
            display_bad_args();
            return;
        }
        display_target()->setCursor(argv[1].i, argv[2].i);
        script_set_result(0);
        return;
    }
    if (!strcmp(command, "print")) {
        if (argc < 2) {
            display_bad_args();
            return;
        }
        for (int i = 1; i < argc; ++i) {
            if (script_val_is_str(argv[i]))
                display_target()->print(script_resolve_str(argv[i]));
            else
                display_target()->print(argv[i].i);
        }
        script_set_result(0);
        return;
    }
    if (!strcmp(command, "fillScreen")) {
        uint32_t color = argc > 1 ? (uint32_t)argv[1].i : TFT_BLACK;

        display_target()->fillScreen(color);
        display_target()->setCursor(0, 0);
        script_set_result(0);
        return;
    }
    if (!strcmp(command, "drawPixel")) {
        if (argc < 4) { display_bad_args(); return; }
        display_target()->drawPixel(
            argv[1].i, argv[2].i, (uint32_t)argv[3].i);
        script_set_result(0);
        return;
    }
    if (!strcmp(command, "drawLine")) {
        if (argc < 6) { display_bad_args(); return; }
        display_target()->drawLine(
            argv[1].i, argv[2].i, argv[3].i, argv[4].i,
            (uint32_t)argv[5].i);
        script_set_result(0);
        return;
    }
    if (!strcmp(command, "drawRect") || !strcmp(command, "fillRect")) {
        if (argc < 6) { display_bad_args(); return; }
        if (command[0] == 'd')
            display_target()->drawRect(
                argv[1].i, argv[2].i, argv[3].i, argv[4].i,
                (uint32_t)argv[5].i);
        else
            display_target()->fillRect(
                argv[1].i, argv[2].i, argv[3].i, argv[4].i,
                (uint32_t)argv[5].i);
        script_set_result(0);
        return;
    }
    if (!strcmp(command, "drawCircle") || !strcmp(command, "fillCircle")) {
        if (argc < 5) { display_bad_args(); return; }
        if (command[0] == 'd')
            display_target()->drawCircle(
                argv[1].i, argv[2].i, argv[3].i,
                (uint32_t)argv[4].i);
        else
            display_target()->fillCircle(
                argv[1].i, argv[2].i, argv[3].i,
                (uint32_t)argv[4].i);
        script_set_result(0);
        return;
    }
    if (!strcmp(command, "setBrightness")) {
        int32_t brightness;

        if (argc < 2) { display_bad_args(); return; }
        brightness = argv[1].i;
        if (brightness < 0) brightness = 0;
        if (brightness > 255) brightness = 255;
        M5Cardputer.Display.setBrightness((uint8_t)brightness);
        script_set_result(brightness);
        return;
    }
    if (!strcmp(command, "width")) {
        script_set_result(M5Cardputer.Display.width());
        return;
    }
    if (!strcmp(command, "height")) {
        script_set_result(M5Cardputer.Display.height());
        return;
    }
    if (!strcmp(command, "beginFrame")) {
        uint32_t color = argc > 1 ? (uint32_t)argv[1].i : TFT_BLACK;

        script_set_result(display_begin_frame(color));
        return;
    }
    if (!strcmp(command, "endFrame")) {
        script_set_result(display_end_frame());
        return;
    }
    if (!strcmp(command, "frameActive")) {
        script_set_result(s_frame_active);
        return;
    }
    if (!strcmp(command, "image")) {
        int status;

        if (argc < 2 || !script_val_is_str(argv[1])) {
            display_bad_args();
            return;
        }
        status = display_image(
            script_resolve_str(argv[1]),
            argc > 2 ? argv[2].i : 0,
            argc > 3 ? argv[3].i : 0,
            argc > 4 ? argv[4].i : 0,
            argc > 5 ? argv[5].i : 0,
            0, 0);
        script_set_result(status);
        return;
    }
    if (!strcmp(command, "loadImage")) {
        if (argc < 2 || !script_val_is_str(argv[1])) {
            display_bad_args();
            return;
        }
        script_set_result(image_load(script_resolve_str(argv[1])));
        return;
    }
    if (!strcmp(command, "imageWidth") ||
        !strcmp(command, "imageHeight")) {
        yajir_image_entry_t *entry;

        if (argc < 2 || script_val_is_str(argv[1])) {
            display_bad_args();
            return;
        }
        entry = image_find(argv[1].i);
        if (!entry)
            script_set_result(-3);
        else
            script_set_result(command[5] == 'W' ? entry->width : entry->height);
        return;
    }
    if (!strcmp(command, "drawImage")) {
        yajir_image_entry_t *entry;
        int status;

        if (argc < 2 || script_val_is_str(argv[1])) {
            display_bad_args();
            return;
        }
        entry = image_find(argv[1].i);
        if (!entry) {
            script_set_result(-3);
            return;
        }
        status = display_image(
            entry->path,
            argc > 2 ? argv[2].i : 0,
            argc > 3 ? argv[3].i : 0,
            argc > 4 ? argv[4].i : entry->width,
            argc > 5 ? argv[5].i : entry->height,
            entry->width, entry->height);
        script_set_result(status);
        return;
    }
    if (!strcmp(command, "releaseImage")) {
        if (argc < 2 || script_val_is_str(argv[1])) {
            display_bad_args();
            return;
        }
        script_set_result(image_release(argv[1].i));
        return;
    }
    if (!strcmp(command, "releaseImages")) {
        memset(s_images, 0, sizeof(s_images));
        script_set_result(0);
        return;
    }
    if (!strcmp(command, "imageSlots")) {
        script_set_result(YAJIR_IMAGE_SLOTS);
        return;
    }

    script_set_result(-1);
}

extern "C" void host_register_all(void)
{
    host_diag_reset();
    s_frame_active = 0;
    memset(s_images, 0, sizeof(s_images));
    s_next_image_id = 1;
    script_register_now(get_tick);
    script_register_stdout(yajir_puts);
    script_register_import(yajir_sd_import);
    reg_in("SD_READY", yajir_sd_ready);
    reg_in("SD_SIZE", yajir_sd_size_mb);
    reg_inout("DISPLAY", NULL, th_display);
    reg_inout("SPEAKER", NULL, th_speaker);
    reg_handler("USB_SERIAL");
    reg_handler("KEYBOARD");

    reg_const("KEY_BACKSPACE", YAJIR_KEY_BACKSPACE);
    reg_const("KEY_TAB", YAJIR_KEY_TAB);
    reg_const("KEY_ENTER", YAJIR_KEY_ENTER);
    reg_const("KEY_MOD_SHIFT", YAJIR_KEY_MOD_SHIFT);
    reg_const("KEY_MOD_CTRL", YAJIR_KEY_MOD_CTRL);
    reg_const("KEY_MOD_ALT", YAJIR_KEY_MOD_ALT);
    reg_const("KEY_MOD_FN", YAJIR_KEY_MOD_FN);
    reg_const("KEY_MOD_OPT", YAJIR_KEY_MOD_OPT);
}

extern "C" void yajir_glue_init(void)
{
    s_previous_key_count = 0;
    s_tft_text_size = 2;
    s_tft_text_color = TFT_WHITE;
    s_tft_text_background = TFT_BLACK;
    s_tft_text_background_set = 0;
    s_frame_active = 0;
}

extern "C" void yajir_glue_stop(void)
{
    M5Cardputer.Speaker.stop();
    s_frame_active = 0;
    s_frame.deleteSprite();
    s_previous_key_count = 0;
}

extern "C" void yajir_glue_poll(int script_running,
                                 yajir_key_input_fn loader_input)
{
    const auto& current = M5Cardputer.Keyboard.keyList();
    const auto& state = M5Cardputer.Keyboard.keysState();
    int32_t modifiers = keyboard_modifiers(state);

    for (const auto& point : current) {
        int32_t character;

        if (was_pressed(point)) continue;
        character = key_character(point, state);
        if (character == 0) continue;

        if (character == 0x03 || !script_running)
            loader_input((uint8_t)character);
        else
            post_keyboard_event(character, modifiers);
    }

    s_previous_key_count =
        current.size() < YAJIR_KEY_TRACK_MAX ? current.size() : YAJIR_KEY_TRACK_MAX;
    for (size_t i = 0; i < s_previous_key_count; ++i)
        s_previous_keys[i] = current[i];
}
