/* yajir_loader.c - USB serial receive, compile, and execution loop */
#include <stddef.h>
#include <string.h>

#include "host_diag.h"
#include "script.h"
#include "vm.h"
#include "yajir_glue.h"
#include "yajir_drive.h"
#include "yajir_loader.h"

#define SCRIPT_MAX 14336

static script_vm_t g_arena;
static char g_source[SCRIPT_MAX];
static size_t g_length;
static size_t g_line_start;
static int g_running;
static int g_overflow_reported;
static int g_swallow_lf;
static int g_echo_swallow_lf;

static void reset_input(void)
{
    memset(g_source, 0, sizeof(g_source));
    g_length = 0;
    g_line_start = 0;
    g_running = 0;
    g_overflow_reported = 0;
    g_swallow_lf = 0;
    g_echo_swallow_lf = 0;
}

static void input_prompt(void)
{
    yajir_puts("Paste a script, then enter @run on its own line.\r\n> ");
}

static void echo_input_byte(uint8_t byte)
{
    if (byte == '\r' || byte == '\n') {
        yajir_puts("\r\n");
    } else {
        yajir_putc((char)byte);
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

static void banner(void)
{
    yajir_puts("\r\n==== Yajir on Raspberry Pi Pico 2 W ====\r\n");
    yajir_puts("Version: ");
    yajir_puts(script_version());
    yajir_puts("\r\nVM arena: ");
    put_size(sizeof(g_arena));
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

static int run_script(const char *source_name)
{
    yajir_putc('[');
    yajir_puts(source_name);
    yajir_puts("] script received: ");
    put_size(g_length);
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

static void start_serial_script(void)
{
    g_source[g_line_start] = '\0';
    g_length = g_line_start;

    if (run_script("loader") != 0)
        yajir_puts("Press Ctrl+C to clear the input and try again.\r\n");
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

void yajir_loader_init(void)
{
    int autorun_status;

    memset(&g_arena, 0, sizeof(g_arena));
    reset_input();
    banner();

    autorun_status = yajir_drive_load_autorun(g_source, sizeof(g_source),
                                               &g_length);
    if (autorun_status == YAJIR_AUTORUN_FOUND) {
        g_line_start = g_length;
        if (run_script("autorun") == 0) return;
        reset_input();
        yajir_puts("[autorun] load failed; falling back to USB serial.\r\n");
    } else if (autorun_status == YAJIR_AUTORUN_TOO_LARGE) {
        yajir_puts("[autorun] autorun.yaj is too large (maximum ");
        put_size(sizeof(g_source) - 1u);
        yajir_puts(" bytes).\r\n");
    } else if (autorun_status == YAJIR_AUTORUN_ERROR) {
        yajir_puts("[autorun] drive read failed.\r\n");
    } else {
        yajir_puts("[autorun] autorun.yaj not found.\r\n");
    }
    input_prompt();
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

    if (g_length >= sizeof(g_source) - 1u) {
        if (!g_overflow_reported) {
            yajir_puts("\r\n[loader] script buffer full; reset to try again.\r\n");
            g_overflow_reported = 1;
        }
        return;
    }

    echo_input_byte(byte);
    g_source[g_length++] = (char)byte;
    g_source[g_length] = '\0';

    if (byte != '\r' && byte != '\n') return;
    if (byte == '\r') g_swallow_lf = 1;

    if (command_at_line("@run")) {
        start_serial_script();
        return;
    }
    if (command_at_line("@fin")) {
        g_source[g_line_start] = '\0';
        g_length = g_line_start;
        yajir_puts("[loader] script stored in RAM, not running.\r\n");
        return;
    }
    g_line_start = g_length;
}

void yajir_loader_tick(void)
{
    if (g_running) script_tick();
}
