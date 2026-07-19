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
#include "yajir_storage.h"

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
    yajir_puts("Paste a script, then enter @run or @save on its own line.\r\n> ");
}

static void echo_input_byte(uint8_t byte)
{
    if (byte == '\r' || byte == '\n') yajir_puts("\r\n");
    else yajir_putc((char)byte);
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

static void put_size(size_t value)
{
    char buf[24];
    int i = 0;
    do {
        buf[i++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value != 0u);
    while (i > 0) yajir_putc(buf[--i]);
}

static int append_source_byte(uint8_t byte)
{
    if (g_length >= sizeof(g_source) - 1u) {
        if (!g_overflow_reported) {
            yajir_puts("\r\n[loader] compressed script buffer full; press Ctrl+C to clear it.\r\n");
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
        if (g_lexical_escape) {
            g_lexical_escape = 0;
        } else if (byte == '\\') {
            g_lexical_escape = 1;
        } else if (byte == '"') {
            g_lexical_state = LEX_NORMAL;
        }
        return;
    }

    if (g_lexical_state == LEX_CHAR) {
        if (g_lexical_escape) {
            g_lexical_escape = 0;
        } else if (byte == '\\') {
            g_lexical_escape = 1;
        } else if (byte == '\'') {
            g_lexical_state = LEX_NORMAL;
        }
        return;
    }

    if (g_lexical_slash) {
        g_lexical_slash = 0;
        if (byte == '/') {
            g_lexical_state = LEX_COMMENT;
            return;
        }
    }

    if (byte == '/') {
        g_lexical_slash = 1;
    } else if (byte == '"') {
        g_lexical_state = LEX_STRING;
    } else if (byte == '\'') {
        g_lexical_state = LEX_CHAR;
    }
}

static void track_source_newline(void)
{
    if (g_lexical_state == LEX_STRING && g_lexical_escape) {
        /* Backslash-newline continues a string and consumes indentation. */
        g_lexical_escape = 0;
    } else {
        /* Reset also provides deterministic recovery for malformed literals. */
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

static void banner(void)
{
    yajir_puts("\r\n==== Yajir on Arduino Nano R4 ====\r\n");
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
    yajir_puts("[loader] load error: line ");
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

static int compile_script(void)
{
    script_init(&g_arena, sizeof(g_arena));
    host_register_all();
    if (script_load(g_source, g_length) != 0) {
        print_load_error();
        return -1;
    }
    return 0;
}

static int run_serial_script(int save)
{
    yajir_puts("[loader] script received: ");
    put_size(g_script_received);
    yajir_puts(" bytes; stored: ");
    put_size(g_length);
    yajir_puts(" bytes; saved: ");
    put_size(g_script_received >= g_length ? g_script_received - g_length : 0u);
    yajir_puts(" bytes\r\n");

    if (compile_script() != 0) return -1;
    if (save) {
        yajir_puts("[autorun] saving compiled script...\r\n");
        if (yajir_storage_save(g_source, g_length) != YAJIR_STORAGE_OK) {
            yajir_puts("[autorun] save failed; previous autorun is no longer valid.\r\n");
            return -1;
        }
        yajir_puts("[autorun] saved: ");
        put_size(g_length);
        yajir_puts(" bytes\r\n");
    }

    g_running = 1;
    yajir_puts("[loader] running. USB input is now sent to ON USB_SERIAL.\r\n");
    return 0;
}

static int try_autorun(void)
{
    size_t length = 0u;
    int status = yajir_storage_load(g_source, sizeof(g_source), &length);

    if (status == YAJIR_STORAGE_EMPTY) {
        yajir_puts("[autorun] no saved script.\r\n");
        return 0;
    }
    if (status != YAJIR_STORAGE_OK) {
        yajir_puts(status == YAJIR_STORAGE_CORRUPT ?
                   "[autorun] saved script is invalid; ignored.\r\n" :
                   "[autorun] data flash read failed.\r\n");
        return 0;
    }

    g_length = length;
    g_script_received = length;
    yajir_puts("[autorun] loaded: ");
    put_size(length);
    yajir_puts(" bytes\r\n");
    if (compile_script() != 0) {
        yajir_puts("[autorun] compile failed; saved script was not started.\r\n");
        return 0;
    }

    g_running = 1;
    yajir_puts("[autorun] running. Send Ctrl+C to stop it.\r\n");
    return 1;
}

static int command_at_line(const char *command)
{
    size_t command_len = strlen(command);
    size_t line_len = g_length - g_line_start;
    while (line_len > 0 &&
           (g_source[g_line_start + line_len - 1] == '\r' ||
            g_source[g_line_start + line_len - 1] == '\n')) {
        --line_len;
    }
    return line_len == command_len &&
           memcmp(g_source + g_line_start, command, command_len) == 0;
}

static void start_serial_script(int save)
{
    g_source[g_line_start] = '\0';
    g_length = g_line_start;
    g_script_received = g_received_line_start;
    if (run_serial_script(save) != 0)
        yajir_puts("Press Ctrl+C to clear the input and try again.\r\n");
}

static void erase_autorun(void)
{
    int status = yajir_storage_erase();

    reset_input();
    yajir_puts(status == YAJIR_STORAGE_OK ?
               "[autorun] saved script erased.\r\n" :
               "[autorun] erase failed.\r\n");
    input_prompt();
}

void yajir_loader_init(void)
{
    memset(&g_arena, 0, sizeof(g_arena));
    reset_input();
    banner();
    if (!try_autorun()) {
        reset_input();
        input_prompt();
    }
}

void yajir_loader_feed_byte(uint8_t byte)
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
        script_post_msg_char("USB_SERIAL", (char)byte);
        return;
    }

    echo_input_byte(byte);
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
        start_serial_script(0);
        return;
    }
    if (g_command_allowed && command_at_line("@save")) {
        start_serial_script(1);
        return;
    }
    if (g_command_allowed && command_at_line("@erase")) {
        erase_autorun();
        return;
    }
    track_source_newline();
    g_line_start = g_length;
    g_received_line_start = g_received;
    begin_source_line();
}

void yajir_loader_tick(void)
{
    if (g_running) script_tick();
}
