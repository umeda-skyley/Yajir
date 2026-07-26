/* yajir_loader.cpp - USB serial receive, compile, and execution loop */
#include "yajir_build_config.h"
#include <Arduino.h>
#include <stddef.h>
#include <string.h>

extern "C" {
#include "script.h"
#include "vm.h"
#include "host_diag.h"
}

#include "yajir_glue.h"
#include "yajir_loader.h"
#include "yajir_sd.h"

static script_vm_t g_arena;
static char g_source[YAJIR_SOURCE_BUFFER_SIZE];
static size_t g_length;
static size_t g_line_start;
static size_t g_received;
static size_t g_received_line_start;
static size_t g_script_received;
static int g_running;
static int g_overflow_reported;
static int g_swallow_lf;
static int g_echo_swallow_lf;

enum lexical_state_t {
    LEX_NORMAL,
    LEX_STRING,
    LEX_CHAR,
    LEX_COMMENT
};

enum line_state_t {
    LINE_PREFIX,
    LINE_PREFIX_SLASH,
    LINE_BODY,
    LINE_COMMENT
};

static lexical_state_t g_lexical_state;
static line_state_t g_line_state;
static int g_lexical_escape;
static int g_lexical_slash;
static int g_command_allowed;

static void reset_input(void)
{
    memset(g_source, 0, sizeof(g_source));
    g_length = 0;
    g_line_start = 0;
    g_received = 0;
    g_received_line_start = 0;
    g_script_received = 0;
    g_running = 0;
    g_overflow_reported = 0;
    g_swallow_lf = 0;
    g_echo_swallow_lf = 0;
    g_lexical_state = LEX_NORMAL;
    g_line_state = LINE_PREFIX;
    g_lexical_escape = 0;
    g_lexical_slash = 0;
    g_command_allowed = 1;
}

static void input_prompt(void)
{
    yajir_puts("Paste script + @run, or enter @load \"name\".\r\n> ");
}

static void echo_byte(uint8_t byte, int local)
{
    /*
     * Script paste echo stays on USB only. Drawing every received byte on the
     * TFT is slow enough to overflow the USB receive queue during bulk paste.
     */
    if (local) {
        if (byte == '\r' || byte == '\n') yajir_puts("\r\n");
        else yajir_putc((char)byte);
    } else {
        if (byte == '\r' || byte == '\n') Serial.write("\r\n");
        else Serial.write(byte);
    }
}

static void echo_runtime_byte(uint8_t byte)
{
    if (g_echo_swallow_lf && byte == '\n') {
        g_echo_swallow_lf = 0;
        return;
    }
    g_echo_swallow_lf = 0;

    if (byte == '\r') {
        yajir_puts("\r\n");
        g_echo_swallow_lf = 1;
    } else if (byte == '\n') {
        yajir_puts("\r\n");
    } else if (byte == 8u || byte == 127u) {
        yajir_puts("\b \b");
    } else {
        yajir_putc((char)byte);
    }
}

static void post_usb_serial_event(uint8_t byte)
{
    char text = (char)byte;
    script_arg_t args[3];

    args[0] = SCRIPT_ARG_INT(byte);
    args[1] = SCRIPT_ARG_INT(0);
    args[2] = SCRIPT_ARG_STR(&text, 1);
    script_post_msg_v("USB_SERIAL", 3, args);
}

static void put_size(size_t value)
{
    char buffer[24];
    int length = 0;

    do {
        buffer[length++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value != 0u);
    while (length > 0) yajir_putc(buffer[--length]);
}

static int append_source_byte(uint8_t byte)
{
    if (g_length >= sizeof(g_source) - 1u) {
        if (!g_overflow_reported) {
            yajir_puts(
                "\r\n[loader] compressed script buffer full; "
                "press Ctrl+C to clear it.\r\n");
            g_overflow_reported = 1;
        }
        return 0;
    }
    g_source[g_length++] = (char)byte;
    g_source[g_length] = '\0';
    return 1;
}

static void track_source_byte(uint8_t byte)
{
    if (g_lexical_state == LEX_COMMENT) return;

    if (g_lexical_state == LEX_STRING) {
        if (g_lexical_escape)
            g_lexical_escape = 0;
        else if (byte == '\\')
            g_lexical_escape = 1;
        else if (byte == '"')
            g_lexical_state = LEX_NORMAL;
        return;
    }

    if (g_lexical_state == LEX_CHAR) {
        if (g_lexical_escape)
            g_lexical_escape = 0;
        else if (byte == '\\')
            g_lexical_escape = 1;
        else if (byte == '\'')
            g_lexical_state = LEX_NORMAL;
        return;
    }

    if (g_lexical_slash) {
        g_lexical_slash = 0;
        if (byte == '/') {
            g_lexical_state = LEX_COMMENT;
            return;
        }
    }

    if (byte == '/')
        g_lexical_slash = 1;
    else if (byte == '"')
        g_lexical_state = LEX_STRING;
    else if (byte == '\'')
        g_lexical_state = LEX_CHAR;
}

static void track_source_newline(void)
{
    if (g_lexical_state == LEX_STRING && g_lexical_escape) {
        /* Backslash-newline continues a string and consumes indentation. */
        g_lexical_escape = 0;
    } else {
        g_lexical_state = LEX_NORMAL;
        g_lexical_escape = 0;
    }
    g_lexical_slash = 0;
}

static void begin_source_line(void)
{
    g_line_state = LINE_PREFIX;
    g_command_allowed = g_lexical_state == LEX_NORMAL;
}

static void store_source_content(uint8_t byte)
{
    if (g_line_state == LINE_COMMENT) return;

    if (g_line_state == LINE_PREFIX) {
        if (byte == ' ' || byte == '\t') return;
        if (g_lexical_state == LEX_NORMAL && byte == '/') {
            g_line_state = LINE_PREFIX_SLASH;
            return;
        }
        g_line_state = LINE_BODY;
    } else if (g_line_state == LINE_PREFIX_SLASH) {
        if (byte == '/') {
            g_line_state = LINE_COMMENT;
            g_lexical_state = LEX_COMMENT;
            return;
        }
        if (append_source_byte('/')) track_source_byte('/');
        g_line_state = LINE_BODY;
    }

    if (append_source_byte(byte)) track_source_byte(byte);
}

static void rebuild_input_state(void)
{
    g_lexical_state = LEX_NORMAL;
    g_line_state = LINE_PREFIX;
    g_lexical_escape = 0;
    g_lexical_slash = 0;
    g_command_allowed = 1;
    g_line_start = 0;

    for (size_t i = 0; i < g_length; ++i) {
        uint8_t byte = (uint8_t)g_source[i];

        if (byte == '\r' || byte == '\n') {
            track_source_newline();
            g_line_start = i + 1u;
            begin_source_line();
            continue;
        }
        if (g_line_state == LINE_PREFIX) {
            if (byte == ' ' || byte == '\t') continue;
            if (g_lexical_state == LEX_NORMAL && byte == '/') {
                g_line_state = LINE_PREFIX_SLASH;
                continue;
            }
            g_line_state = LINE_BODY;
        } else if (g_line_state == LINE_PREFIX_SLASH) {
            if (byte == '/') {
                g_line_state = LINE_COMMENT;
                g_lexical_state = LEX_COMMENT;
                continue;
            }
            track_source_byte('/');
            g_line_state = LINE_BODY;
        }
        track_source_byte(byte);
    }
}

static void banner(void)
{
    yajir_puts("\r\n==== Yajir on M5Stack Cardputer ====\r\n");
    yajir_puts("Version: ");
    yajir_puts(script_version());
    yajir_puts("\r\nVM arena: ");
    put_size(sizeof(g_arena));
    yajir_puts(" bytes\r\nSource buffer: ");
    put_size(sizeof(g_source));
    yajir_puts(" bytes\r\n");
}

static void print_load_error(void)
{
    const script_error_t *error = script_last_error();

    yajir_puts("[loader] load error: ");
    if (error->src_name[0]) {
        yajir_puts("in library '");
        yajir_puts(error->src_name);
        yajir_puts("' ");
    }
    yajir_puts("line ");
    put_size(error->line > 0 ? (size_t)error->line : 0u);
    yajir_puts(": ");
    yajir_puts(script_strerror(error->code));
    if (error->tok[0]) {
        yajir_puts(" '");
        yajir_puts(error->tok);
        yajir_putc('\'');
    }
    if (error->code == ERR_END_EXPECTED && error->aux > 0) {
        yajir_puts(" (block opened at line ");
        put_size((size_t)error->aux);
        yajir_putc(')');
    }
    if ((error->code == ERR_UNKNOWN_PORT || error->code == ERR_UNKNOWN_NAME) &&
        error->tok[0]) {
        const char *suggestion = host_suggest_name(error->tok);
        if (suggestion) {
            yajir_puts(" (did you mean '");
            yajir_puts(suggestion);
            yajir_puts("'?)");
        }
    }
    yajir_puts("\r\n");
}

static int command_at_line(const char *command)
{
    size_t command_length = strlen(command);
    size_t line_length = g_length - g_line_start;

    while (line_length > 0 &&
           (g_source[g_line_start + line_length - 1] == '\r' ||
            g_source[g_line_start + line_length - 1] == '\n')) {
        --line_length;
    }
    return line_length == command_length &&
           memcmp(g_source + g_line_start, command, command_length) == 0;
}

static int load_command_at_line(char *name, size_t capacity)
{
    const char *line = g_source + g_line_start;
    size_t line_length = g_length - g_line_start;
    size_t position = 5u;
    size_t name_length = 0;

    while (line_length > 0 &&
           (line[line_length - 1u] == '\r' ||
            line[line_length - 1u] == '\n'))
        --line_length;

    if (line_length < 5u || memcmp(line, "@load", 5u) != 0)
        return 0;
    if (line_length > 5u && line[5] != ' ' && line[5] != '\t')
        return 0;

    while (position < line_length &&
           (line[position] == ' ' || line[position] == '\t'))
        ++position;
    if (position >= line_length || line[position++] != '"') return -1;

    while (position < line_length && line[position] != '"') {
        if (name_length + 1u >= capacity) return -1;
        name[name_length++] = line[position++];
    }
    if (position >= line_length || name_length == 0u) return -1;
    name[name_length] = '\0';
    ++position;

    while (position < line_length &&
           (line[position] == ' ' || line[position] == '\t'))
        ++position;
    return position == line_length ? 1 : -1;
}

static int run_script(const char *source_name)
{
    yajir_putc('[');
    yajir_puts(source_name);
    yajir_puts("] script received: ");
    if (!strcmp(source_name, "loader")) {
        put_size(g_script_received);
        yajir_puts(" bytes; stored: ");
        put_size(g_length);
        yajir_puts(" bytes; saved: ");
        put_size(g_script_received >= g_length ?
                 g_script_received - g_length : 0u);
    } else {
        put_size(g_length);
    }
    yajir_puts(" bytes\r\n");

    script_init(&g_arena, sizeof(g_arena));
    host_register_all();
    if (script_load(g_source, g_length) != 0) {
        print_load_error();
        return -1;
    }

    g_running = 1;
    yajir_putc('[');
    yajir_puts(source_name);
    yajir_puts("] running. USB input is now sent to ON USB_SERIAL.\r\n");
    return 0;
}

static void start_script(void)
{
    g_source[g_line_start] = '\0';
    g_length = g_line_start;
    g_script_received = g_received_line_start;

    if (run_script("loader") != 0)
        yajir_puts("Press Ctrl+C to clear the input and try again.\r\n");
}

static void load_sd_script(const char *name)
{
    size_t length = 0;
    int status;

    if (!yajir_sd_ready() && yajir_sd_init() != 0) {
        reset_input();
        yajir_puts("[load] microSD is not available.\r\n");
        input_prompt();
        return;
    }

    status = yajir_sd_load_script(name, g_source, sizeof(g_source), &length);
    if (status != YAJIR_SD_FOUND) {
        reset_input();
        if (status == YAJIR_SD_NOT_FOUND)
            yajir_puts("[load] script not found.\r\n");
        else if (status == YAJIR_SD_TOO_LARGE)
            yajir_puts("[load] script is too large.\r\n");
        else if (status == YAJIR_SD_INVALID_NAME)
            yajir_puts("[load] invalid 8.3 script name.\r\n");
        else
            yajir_puts("[load] failed to read script.\r\n");
        input_prompt();
        return;
    }

    g_length = length;
    g_line_start = length;
    yajir_puts("[load] loaded /");
    yajir_puts(name);
    if (strchr(name, '.') == NULL) yajir_puts(".yaj");
    yajir_puts("\r\n");
    if (run_script("load") != 0)
        yajir_puts("Press Ctrl+C to clear the input and try again.\r\n");
}

void yajir_loader_init(void)
{
    size_t autorun_length = 0;
    int autorun_status;

    memset(&g_arena, 0, sizeof(g_arena));
    reset_input();
    banner();

    if (yajir_sd_init() != 0) {
        yajir_puts("[sd] no card or mount failed; USB loader remains available.\r\n");
        input_prompt();
        return;
    }

    yajir_puts("[sd] ready: ");
    put_size((size_t)yajir_sd_size_mb());
    yajir_puts(" MB\r\n");
    autorun_status = yajir_sd_load_autorun(
        g_source, sizeof(g_source), &autorun_length);
    if (autorun_status == YAJIR_SD_FOUND) {
        g_length = autorun_length;
        g_line_start = g_length;
        yajir_puts("[autorun] loaded /autorun.yaj\r\n");
        if (run_script("autorun") == 0) return;
        yajir_puts("[autorun] compile failed; falling back to USB serial.\r\n");
        reset_input();
    } else if (autorun_status == YAJIR_SD_TOO_LARGE) {
        yajir_puts("[autorun] /autorun.yaj is too large (maximum ");
        put_size(sizeof(g_source) - 1u);
        yajir_puts(" bytes).\r\n");
    } else if (autorun_status == YAJIR_SD_ERROR) {
        yajir_puts("[autorun] failed to read /autorun.yaj.\r\n");
    } else {
        yajir_puts("[autorun] /autorun.yaj not found.\r\n");
    }
    input_prompt();
}

static void feed_byte(uint8_t byte, int local)
{
    if (byte == 0x03u) {
        int was_running = g_running;

        if (was_running) yajir_glue_stop();
        reset_input();
        yajir_puts("\r\n^C\r\n");
        yajir_puts(was_running ? "[loader] script stopped.\r\n"
                               : "[loader] input cleared.\r\n");
        input_prompt();
        return;
    }

    if (g_swallow_lf && byte == '\n') {
        g_swallow_lf = 0;
        return;
    }
    g_swallow_lf = 0;

    if (g_running) {
        echo_runtime_byte(byte);
        post_usb_serial_event(byte);
        return;
    }

    if (byte == 8u || byte == 127u) {
        if (g_received > g_received_line_start) {
            --g_received;
            if (g_length > g_line_start) {
                --g_length;
                g_source[g_length] = '\0';
            }
            rebuild_input_state();
            if (local) yajir_puts("\b \b");
            else Serial.write("\b \b");
        }
        return;
    }

    echo_byte(byte, local);
    ++g_received;

    if (byte != '\r' && byte != '\n') {
        store_source_content(byte);
        return;
    }
    if (byte == '\r') g_swallow_lf = 1;

    if (g_line_state == LINE_PREFIX_SLASH) {
        if (append_source_byte('/')) track_source_byte('/');
    }
    append_source_byte(byte);

    if (g_command_allowed && command_at_line("@run")) {
        start_script();
        return;
    }
    {
        char name[16];
        int load_command = g_command_allowed ?
            load_command_at_line(name, sizeof(name)) : 0;

        if (load_command > 0) {
            load_sd_script(name);
            return;
        }
        if (load_command < 0) {
            reset_input();
            yajir_puts("[load] usage: @load \"name\"\r\n");
            input_prompt();
            return;
        }
    }
    track_source_newline();
    g_line_start = g_length;
    g_received_line_start = g_received;
    begin_source_line();
}

void yajir_loader_feed_byte(uint8_t byte)
{
    feed_byte(byte, 0);
}

void yajir_loader_feed_key(uint8_t byte)
{
    feed_byte(byte, 1);
}

void yajir_loader_tick(void)
{
    yajir_glue_poll(g_running, yajir_loader_feed_key);
    if (g_running) script_tick();
}
