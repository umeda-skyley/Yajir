/* yajir_loader.c - USB serial receive, compile, and execution loop */
#include <stddef.h>
#include <string.h>

#include "host_diag.h"
#include "script.h"
#include "vm.h"
#include "yajir_glue.h"
#include "yajir_loader.h"

#define SCRIPT_MAX 16384

static script_vm_t g_arena;
static char g_source[SCRIPT_MAX];
static size_t g_length;
static size_t g_line_start;
static int g_running;
static int g_overflow_reported;
static int g_swallow_lf;

static void reset_input(void)
{
    memset(g_source, 0, sizeof(g_source));
    g_length = 0;
    g_line_start = 0;
    g_running = 0;
    g_overflow_reported = 0;
    g_swallow_lf = 0;
}

static void input_prompt(void)
{
    yajir_puts("Paste a script, then enter @run on its own line.\r\n> ");
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
    input_prompt();
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

static void start_script(void)
{
    g_source[g_line_start] = '\0';
    g_length = g_line_start;

    yajir_puts("[loader] script received: ");
    put_size(g_length);
    yajir_puts(" bytes\r\n");

    script_init(&g_arena, sizeof(g_arena));
    host_register_all();
    if (script_load(g_source, g_length) != 0) {
        print_load_error();
        yajir_puts("Press Ctrl+C to clear the input and try again.\r\n");
        return;
    }

    g_running = 1;
    yajir_puts("[loader] running. USB input is now sent to ON USB_SERIAL.\r\n");
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
    memset(&g_arena, 0, sizeof(g_arena));
    reset_input();
    banner();
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

    g_source[g_length++] = (char)byte;
    g_source[g_length] = '\0';

    if (byte != '\r' && byte != '\n') return;
    if (byte == '\r') g_swallow_lf = 1;

    if (command_at_line("@run")) {
        start_script();
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
