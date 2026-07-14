/* fileutil.c - PC extension file utility ports
 *
 * FILE_READER: "path" -> FILE_READER
 *   SRESULT = file body, RESULT = bytes kept in SRESULT.
 *
 * FILE_WRITER: "path", "body" [, "w"|"a"|"wb"|"ab"] -> FILE_WRITER
 *   RESULT = source bytes written, or a negative error code.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "script.h"
#include "vm.h"
#include "host_diag.h"
#include "fileutil.h"

#define FILEUTIL_ERR_BAD_ARGS   (-1)
#define FILEUTIL_ERR_OPEN       (-2)
#define FILEUTIL_ERR_IO         (-3)

static int is_file_mode(const char *s)
{
    return s &&
           (strcmp(s, "w") == 0 ||
            strcmp(s, "a") == 0 ||
            strcmp(s, "wb") == 0 ||
            strcmp(s, "ab") == 0);
}

static FILE *open_utf8_path(const char *path, const char *mode)
{
    int pn, mn;
    WCHAR *wpath;
    WCHAR wmode[8];
    FILE *f;

    if (!path || !path[0] || !mode || !mode[0]) return NULL;

    pn = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    mn = MultiByteToWideChar(CP_UTF8, 0, mode, -1, NULL, 0);
    if (pn <= 0 || mn <= 0 || mn > (int)(sizeof(wmode) / sizeof(wmode[0]))) return NULL;

    wpath = (WCHAR *)malloc((size_t)pn * sizeof(WCHAR));
    if (!wpath) return NULL;

    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, pn);
    MultiByteToWideChar(CP_UTF8, 0, mode, -1, wmode, mn);
    f = _wfopen(wpath, wmode);
    free(wpath);
    return f;
}

static void th_file_reader(int argc, const script_value_t *a)
{
    const char *path;
    FILE *f;
    char *buf;
    size_t n;
    int truncated = 0;

    script_set_result(0);
    script_set_sresult("");

    if (argc < 1 || !script_val_is_str(a[0])) {
        script_set_result(FILEUTIL_ERR_BAD_ARGS);
        return;
    }

    path = script_resolve_str(a[0]);
    if (!path || !path[0]) {
        script_set_result(FILEUTIL_ERR_BAD_ARGS);
        return;
    }

    f = open_utf8_path(path, "rb");
    if (!f) {
        script_set_result(FILEUTIL_ERR_OPEN);
        return;
    }

    buf = (char *)malloc(CFG_SSTR_LEN);
    if (!buf) {
        fclose(f);
        script_set_result(FILEUTIL_ERR_IO);
        return;
    }

    n = fread(buf, 1, CFG_SSTR_LEN - 1, f);
    if (n < CFG_SSTR_LEN - 1) {
        if (ferror(f)) {
            free(buf);
            fclose(f);
            script_set_result(FILEUTIL_ERR_IO);
            return;
        }
    } else {
        int c = fgetc(f);
        if (c != EOF) truncated = 1;
        else if (ferror(f)) {
            free(buf);
            fclose(f);
            script_set_result(FILEUTIL_ERR_IO);
            return;
        }
    }

    buf[n] = '\0';
    if (truncated) vm_set_err(ERR_STR_TRUNC);
    script_set_result((int32_t)n);
    script_set_sresult(buf);

    free(buf);
    fclose(f);
}

static void th_file_writer(int argc, const script_value_t *a)
{
    const char *path;
    const char *body;
    const char *mode = "w";
    size_t len;
    size_t written;
    FILE *f;

    script_set_result(0);

    if (argc < 2 || !script_val_is_str(a[0]) || !script_val_is_str(a[1])) {
        script_set_result(FILEUTIL_ERR_BAD_ARGS);
        return;
    }

    path = script_resolve_str(a[0]);
    body = script_resolve_str(a[1]);
    if (!path || !path[0] || !body) {
        script_set_result(FILEUTIL_ERR_BAD_ARGS);
        return;
    }

    if (argc >= 3) {
        if (!script_val_is_str(a[2])) {
            script_set_result(FILEUTIL_ERR_BAD_ARGS);
            return;
        }
        if (is_file_mode(body)) {
            script_set_result(FILEUTIL_ERR_BAD_ARGS);
            return;
        }
        mode = script_resolve_str(a[2]);
        if (!is_file_mode(mode)) {
            script_set_result(FILEUTIL_ERR_BAD_ARGS);
            return;
        }
    }

    f = open_utf8_path(path, mode);
    if (!f) {
        script_set_result(FILEUTIL_ERR_OPEN);
        return;
    }

    len = strlen(body);
    written = fwrite(body, 1, len, f);
    if (fclose(f) != 0 || written != len) {
        script_set_result(FILEUTIL_ERR_IO);
        return;
    }

    script_set_result((int32_t)written);
}

void register_fileutils(void)
{
    script_register_inout("FILE_READER", NULL, th_file_reader, SCRIPT_T_STR);
    script_register_inout("FILE_WRITER", NULL, th_file_writer, SCRIPT_T_INT);
    host_diag_note("FILE_READER");
    host_diag_note("FILE_WRITER");
}
