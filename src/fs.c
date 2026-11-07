/* SPDX-License-Identifier: MIT
 * Copyright 2026 the Desktop Commander C contributors
 *
 * Filesystem tools: read_file, read_multiple_files, write_file,
 * create_directory, list_directory, move_file, get_file_info.
 */
#include "config.h"
#include "tools.h"
#include "util.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* A name from readdir fits in d_name, which is 256 bytes on the systems this
 * server runs on. A listing therefore needs one block, not one allocation for
 * each entry. */
#define NAME_MAX_BYTES 256

tool_result result_text(jdoc *d, const char *text)
{
    tool_result r = {jarr(d), JNULL, false};
    jref block = jobj(d);
    jobj_set(d, block, "type", jstr(d, "text"));
    jobj_set(d, block, "text", jstr(d, text ? text : ""));
    jarr_push(d, r.content, block);
    return r;
}

tool_result result_error(jdoc *d, const char *fmt, ...)
{
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    tool_result r = result_text(d, msg);
    r.is_error = true;
    return r;
}

tool_result result_text_struct(jdoc *d, const char *text, jref structured)
{
    tool_result r = result_text(d, text);
    r.structured = structured;
    return r;
}

tool_result result_buf(jdoc *d, sbuf *b)
{
    tool_result r = result_text(d, b->p ? b->p : "");
    sb_free(b);
    return r;
}

/* ---------------------------------------------------------------- base64 */

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* Encodes into dst, which holds ((len + 2) / 3) * 4 + 1 bytes. */
static void base64_encode(const unsigned char *data, size_t len, char *dst)
{
    size_t j = 0;
    for (size_t i = 0; i < len; i += 3) {
        unsigned v = (unsigned)data[i] << 16;
        if (i + 1 < len)
            v |= (unsigned)data[i + 1] << 8;
        if (i + 2 < len)
            v |= data[i + 2];
        dst[j++] = B64[(v >> 18) & 0x3F];
        dst[j++] = B64[(v >> 12) & 0x3F];
        dst[j++] = (i + 1 < len) ? B64[(v >> 6) & 0x3F] : '=';
        dst[j++] = (i + 2 < len) ? B64[v & 0x3F] : '=';
    }
    dst[j] = '\0';
}

/* ---------------------------------------------------------------- reading */

/* Join the lines of a range with one \n each and no trailing newline, the
 * way the original server's readline-based reader returns them. */
static char *join_lines_clean(const char *data, size_t len)
{
    sbuf b;
    sb_init(&b);
    size_t i = 0;
    while (i < len) {
        size_t start = i;
        while (i < len && data[i] != '\n')
            i++;
        size_t end = i;
        if (end > start && data[end - 1] == '\r')
            end--;
        if (i < len)
            i++; /* the newline */
        if (start == end && i >= len && b.n == 0)
            break; /* an empty file, or only a final newline */
        if (b.n || start != 0)
            sb_putc(&b, '\n');
        sb_putn(&b, data + start, end - start);
    }
    return sb_take(&b);
}

/* Copy lines [offset, offset+length) of a text into a new string.
 * Returns the count of lines returned. */
static char *slice_lines(const char *text, int64_t offset, int64_t length,
                         int64_t total_lines, int64_t *count_out, bool *tail_out)
{
    *tail_out = offset < 0;
    *count_out = 0;
    if (!text)
        return xstrdup("");

    if (offset < 0) {
        /* Tail: the last |offset| lines. */
        int64_t want = -offset;
        if (want > total_lines)
            want = total_lines;
        int64_t start = total_lines - want;
        size_t skip = start > 0 ? bytes_for_lines(text, (size_t)start) : 0;
        *count_out = want;
        return join_lines_clean(text + skip, strlen(text + skip));
    }

    if (length < 0)
        length = 0;
    size_t start = offset > 0 ? bytes_for_lines(text, (size_t)offset) : 0;
    if (start >= strlen(text))
        return xstrdup("");

    const char *p = text + start;
    int64_t n = 0;
    size_t bytes = 0;
    while (*p && n < length) {
        const char *nl = strchr(p, '\n');
        if (!nl) {
            bytes = strlen(p);
            n++;
            break;
        }
        bytes += (size_t)(nl - p) + 1;
        p = nl + 1;
        n++;
    }
    *count_out = n;
    return join_lines_clean(text + start, bytes);
}

tool_result tool_read_file(jdoc *d, jref a)
{
    const char *path = jstr_get(d, a, "path", NULL);
    if (!path || !*path)
        return result_error(d, "Missing path");

    if (jbool_get(d, a, "isUrl", false))
        return result_error(d, "URL reads are not supported in this build; "
                               "fetch with start_process (curl/wget).");

    const config *c = cfg();
    int64_t length = jint_get(d, a, "length", c->read_line_limit);
    int64_t offset = jint_get(d, a, "offset", 0);

    char *abs = NULL;
    char *err = validate_path(path, &abs);
    if (err) {
        tool_result r = result_error(d, "%s", err);
        free(err);
        return r;
    }

    struct stat st;
    if (stat(abs, &st) != 0) {
        tool_result r = result_error(d, "Cannot read %s: %s", path, strerror(errno));
        free(abs);
        return r;
    }

    if (S_ISDIR(st.st_mode)) {
        /* Same fallback as the original: a listing with a hint. */
        jref args = jobj(d);
        jobj_set(d, args, "path", jstr(d, path));
        tool_result listed = tool_list_directory(d, args);
        sbuf b;
        sb_init(&b);
        sb_put(&b, "This is a directory, not a file. Use list_directory instead.\n\n");
        sb_put(&b, jstr_get(d, jidx(d, listed.content, 0), "text", ""));
        tool_result r = result_buf(d, &b);
        free(abs);
        return r;
    }

    size_t len = 0;
    char *data = read_file_all(abs, &len, &err);
    if (!data) {
        tool_result r = result_error(d, "%s", err);
        free(err);
        free(abs);
        return r;
    }

    /* Images travel as an image block so the model can see them. */
    if (file_is_image(abs)) {
        size_t blen = ((len + 2) / 3) * 4 + 1;
        char *b64 = xmalloc(blen);
        base64_encode((const unsigned char *)data, len, b64);
        jref content = jarr(d);
        jref summary = jobj(d);
        jobj_set(d, summary, "type", jstr(d, "text"));
        char head[PATH_MAX + 64];
        snprintf(head, sizeof head, "Image file: %s (%s)", path, mime_type_for(abs));
        jobj_set(d, summary, "text", jstr(d, head));
        jarr_push(d, content, summary);
        jref img = jobj(d);
        jobj_set(d, img, "type", jstr(d, "image"));
        jobj_set(d, img, "data", jstr(d, b64));
        jobj_set(d, img, "mimeType", jstr(d, mime_type_for(abs)));
        jarr_push(d, content, img);
        free(b64);
        free(data);
        free(abs);
        return (tool_result){content, JNULL, false};
    }

    if (data_is_binary(data, len)) {
        free(data);
        free(abs);
        return result_error(d, "Cannot read %s as text: the file is binary", path);
    }

    int64_t total = (int64_t)count_lines(data);
    int64_t count = 0;
    bool tail = false;
    char *slice = slice_lines(data, offset, length, total, &count, &tail);
    free(data);

    sbuf b;
    sb_init(&b);
    if (tail) {
        sb_printf(&b, "[Reading last %lld lines (total: %lld lines)]\n\n",
                  (long long)count, (long long)total);
    } else {
        int64_t remaining = total - (offset + count);
        if (remaining < 0)
            remaining = 0;
        if (offset == 0)
            sb_printf(&b,
                      "[Reading %lld lines from start (total: %lld lines, %lld remaining)]\n\n",
                      (long long)count, (long long)total, (long long)remaining);
        else
            sb_printf(&b,
                      "[Reading %lld lines from line %lld (total: %lld lines, %lld remaining)]\n\n",
                      (long long)count, (long long)offset, (long long)total,
                      (long long)remaining);
    }
    sb_put(&b, slice);

    tool_result r = result_buf(d, &b);
    free(slice);
    free(abs);
    return r;
}

tool_result tool_read_multiple_files(jdoc *d, jref a)
{
    jref paths = jget(d, a, "paths");
    if (!jis_arr(d, paths) || jlen(d, paths) == 0)
        return result_error(d, "paths must be a non-empty array of strings");

    sbuf summary;
    sb_init(&summary);
    jref content = jarr(d);

    for (size_t i = 0; i < jlen(d, paths); i++) {
        const char *path = jstr_of(d, jidx(d, paths, i));
        char *abs = NULL;
        char *err = validate_path(path, &abs);
        if (err) {
            if (i)
                sb_putc(&summary, '\n');
            sb_printf(&summary, "%s: Error - %s", path, err);
            free(err);
            continue;
        }
        size_t len = 0;
        char *data = read_file_all(abs, &len, &err);
        if (!data) {
            if (i)
                sb_putc(&summary, '\n');
            sb_printf(&summary, "%s: Error - %s", path, err);
            free(err);
            free(abs);
            continue;
        }
        if (i)
            sb_putc(&summary, '\n');
        if (file_is_image(abs)) {
            sb_printf(&summary, "%s: %s (image)", path, mime_type_for(abs));
            jref header = jobj(d);
            jobj_set(d, header, "type", jstr(d, "text"));
            sbuf h;
            sb_init(&h);
            sb_printf(&h, "\n--- %s contents: ---\n", path);
            jobj_set(d, header, "text", jstrn(d, h.p, h.n));
            sb_free(&h);
            jarr_push(d, content, header);
            size_t blen = ((len + 2) / 3) * 4 + 1;
            char *b64 = xmalloc(blen);
            base64_encode((const unsigned char *)data, len, b64);
            jref img = jobj(d);
            jobj_set(d, img, "type", jstr(d, "image"));
            jobj_set(d, img, "data", jstr(d, b64));
            jobj_set(d, img, "mimeType", jstr(d, mime_type_for(abs)));
            jarr_push(d, content, img);
            free(b64);
        } else {
            sb_printf(&summary, "%s: %s (text)", path, mime_type_for(abs));
            sbuf body;
            sb_init(&body);
            sb_printf(&body, "\n--- %s contents: ---\n", path);
            sb_putn(&body, data, strlen(data));
            jref block = jobj(d);
            jobj_set(d, block, "type", jstr(d, "text"));
            jobj_set(d, block, "text", jstrn(d, body.p, body.n));
            jarr_push(d, content, block);
            sb_free(&body);
        }
        free(data);
        free(abs);
    }

    /* The summary block goes first, like the original. */
    jref all = jarr(d);
    jref head = jobj(d);
    jobj_set(d, head, "type", jstr(d, "text"));
    jobj_set(d, head, "text", jstrn(d, summary.p ? summary.p : "", summary.n));
    jarr_push(d, all, head);
    for (size_t i = 0; i < jlen(d, content); i++)
        jarr_push(d, all, jidx(d, content, i));
    sb_free(&summary);
    return (tool_result){all, JNULL, false};
}

/* ---------------------------------------------------------------- writing */

tool_result tool_write_file(jdoc *d, jref a)
{
    const char *path = jstr_get(d, a, "path", NULL);
    if (!path || !*path)
        return result_error(d, "Missing path");
    jref cv = jget(d, a, "content");
    if (!jis_str(d, cv))
        return result_error(d, "content must be a string");
    const char *text = jstr_of(d, cv);

    bool mode_given = jget(d, a, "mode") != JNULL;
    const char *mode = jstr_get(d, a, "mode", "rewrite");
    bool append = strcmp(mode, "append") == 0;
    if (!append && strcmp(mode, "rewrite") != 0)
        return result_error(d, "mode must be 'rewrite' or 'append'");

    char *abs = NULL;
    char *err = validate_path(path, &abs);
    if (err) {
        tool_result r = result_error(d, "%s", err);
        free(err);
        return r;
    }

    if (!mode_given) {
        struct stat st;
        if (stat(abs, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0) {
            tool_result r = result_error(
                d,
                "Write rejected: %s already has content (%lld bytes) and no mode was given. "
                "Retry with mode 'append' or mode 'rewrite'.",
                path, (long long)st.st_size);
            free(abs);
            return r;
        }
    }

    int64_t lines = (int64_t)count_lines(text);
    char *out = write_file_atomic(abs, text, strlen(text), append, 0644, &err);
    if (!out) {
        tool_result r = result_error(d, "%s", err);
        free(err);
        free(abs);
        return r;
    }
    free(out);

    char msg[PATH_MAX + 64];
    snprintf(msg, sizeof msg, "Successfully %s %s (%lld lines)",
             append ? "appended to" : "wrote to", path, (long long)lines);
    tool_result r = result_text(d, msg);
    free(abs);
    return r;
}

tool_result tool_create_directory(jdoc *d, jref a)
{
    const char *path = jstr_get(d, a, "path", NULL);
    if (!path || !*path)
        return result_error(d, "Missing path");

    char *abs = NULL;
    char *err = validate_path(path, &abs);
    if (err) {
        tool_result r = result_error(d, "%s", err);
        free(err);
        return r;
    }

    /* mkdir -p */
    char *tmp = xstrdup(abs);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        tool_result r = result_error(d, "Cannot create %s: %s", path, strerror(errno));
        free(tmp);
        free(abs);
        return r;
    }
    free(tmp);
    free(abs);
    char msg[PATH_MAX + 64];
    snprintf(msg, sizeof msg, "Successfully created directory %s", path);
    return result_text(d, msg);
}

tool_result tool_move_file(jdoc *d, jref a)
{
    const char *src = jstr_get(d, a, "source", NULL);
    const char *dst = jstr_get(d, a, "destination", NULL);
    if (!src || !dst)
        return result_error(d, "source and destination are required");

    char *asrc = NULL, *adst = NULL;
    char *err = validate_path(src, &asrc);
    if (!err)
        err = validate_path(dst, &adst);
    if (err) {
        tool_result r = result_error(d, "%s", err);
        free(err);
        free(asrc);
        return r;
    }
    if (rename(asrc, adst) != 0) {
        tool_result r = result_error(d, "Cannot move %s to %s: %s", src, dst, strerror(errno));
        free(asrc);
        free(adst);
        return r;
    }
    free(asrc);
    free(adst);
    sbuf b;
    sb_init(&b);
    sb_printf(&b, "Successfully moved %s to %s", src, dst);
    tool_result r = result_buf(d, &b);
    return r;
}

/* ---------------------------------------------------------------- listing */

static int cmp_names(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

/* One line per entry, joined by \n in `out`. Names live in one block of
 * NAME_MAX_BYTES slots so a directory sort costs one allocation. */
static void list_recursive(jdoc *d, sbuf *out, const char *dir, int depth, const char *rel,
                           bool top)
{
    if (depth <= 0)
        return;
    DIR *dh = opendir(dir);
    if (!dh) {
        const char *shown = (rel && *rel) ? rel : base_name(dir);
        if (out->n)
            sb_putc(out, '\n');
        sb_printf(out, "%s %s — %s", errno == ENOENT ? "[NOT_FOUND]" : "[DENIED]", shown,
                  errno == ENOENT ? "path does not exist" : "not accessible");
        return;
    }

    char *names = NULL;
    size_t n = 0, cap = 0;
    struct dirent *de;
    while ((de = readdir(dh)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 32;
            names = xrealloc(names, cap * NAME_MAX_BYTES);
        }
        snprintf(names + n * NAME_MAX_BYTES, NAME_MAX_BYTES, "%s", de->d_name);
        n++;
    }
    closedir(dh);

    if (n > 1)
        qsort(names, n, NAME_MAX_BYTES, cmp_names);

    size_t show = n;
    size_t hidden = 0;
    if (!top && n > 100) {
        show = 100;
        hidden = n - 100;
    }

    for (size_t i = 0; i < show; i++) {
        const char *name = names + i * NAME_MAX_BYTES;
        char full[PATH_MAX];
        snprintf(full, sizeof full, "%s/%s", dir, name);
        struct stat st;
        bool isdir = false;
        if (lstat(full, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                isdir = true;
            } else if (S_ISLNK(st.st_mode)) {
                struct stat target;
                if (stat(full, &target) == 0 && S_ISDIR(target.st_mode))
                    isdir = true;
            }
        }
        if (out->n)
            sb_putc(out, '\n');
        sb_printf(out, "%s %s%s%s", isdir ? "[DIR]" : "[FILE]", (rel && *rel) ? rel : "",
                  (rel && *rel) ? "/" : "", name);

        if (isdir && depth > 1) {
            char relname[PATH_MAX];
            snprintf(relname, sizeof relname, "%s%s%s", (rel && *rel) ? rel : "",
                     (rel && *rel) ? "/" : "", name);
            char *checked = NULL;
            char *verr = validate_path(full, &checked);
            if (!verr)
                list_recursive(d, out, checked, depth - 1, relname, false);
            free(checked);
            free(verr);
        }
    }

    if (hidden) {
        const char *shown = (rel && *rel) ? rel : base_name(dir);
        sb_putc(out, '\n');
        sb_printf(out, "[WARNING] %s: %zu items hidden (showing first 100 of %zu total)",
                  shown, hidden, n);
    }
    free(names);
}

tool_result tool_list_directory(jdoc *d, jref a)
{
    const char *path = jstr_get(d, a, "path", NULL);
    if (!path || !*path)
        return result_error(d, "Missing path");
    int64_t depth = jint_get(d, a, "depth", 2);
    if (depth < 1)
        depth = 1;

    char *abs = NULL;
    char *err = validate_path(path, &abs);
    if (err) {
        tool_result r = result_error(d, "%s", err);
        free(err);
        return r;
    }

    sbuf b;
    sb_init(&b);
    struct stat st;
    if (stat(abs, &st) == 0 && S_ISREG(st.st_mode)) {
        sb_printf(&b, "[FILE] %s", base_name(abs));
    } else {
        list_recursive(d, &b, abs, (int)depth, "", true);
    }
    tool_result r = result_buf(d, &b);
    free(abs);
    return r;
}

/* ---------------------------------------------------------------- info */

static const char *iso_time(time_t t, char *buf, size_t n)
{
    struct tm tm;
    if (gmtime_r(&t, &tm) == NULL)
        return "unknown";
    strftime(buf, n, "%Y-%m-%dT%H:%M:%S.000Z", &tm);
    return buf;
}

tool_result tool_get_file_info(jdoc *d, jref a)
{
    const char *path = jstr_get(d, a, "path", NULL);
    if (!path || !*path)
        return result_error(d, "Missing path");

    char *abs = NULL;
    char *err = validate_path(path, &abs);
    if (err) {
        tool_result r = result_error(d, "%s", err);
        free(err);
        return r;
    }
    struct stat st;
    if (stat(abs, &st) != 0) {
        tool_result r = result_error(d, "Cannot stat %s: %s", path, strerror(errno));
        free(abs);
        return r;
    }

    char cbuf[64], mbuf[64], abuf[64];
#ifdef __APPLE__
    const char *created = iso_time(st.st_birthtime, cbuf, sizeof cbuf);
#else
    const char *created = iso_time(st.st_ctime, cbuf, sizeof cbuf);
#endif
    const char *modified = iso_time(st.st_mtime, mbuf, sizeof mbuf);
    const char *accessed = iso_time(st.st_atime, abuf, sizeof abuf);
    char perms[8];
    snprintf(perms, sizeof perms, "%03o", (unsigned)(st.st_mode & 0777));
    bool isdir = S_ISDIR(st.st_mode);

    sbuf b;
    sb_init(&b);
    sb_printf(&b, "size: %lld\n", (long long)st.st_size);
    sb_printf(&b, "created: %s\n", created);
    sb_printf(&b, "modified: %s\n", modified);
    sb_printf(&b, "accessed: %s\n", accessed);
    sb_printf(&b, "isDirectory: %s\n", isdir ? "true" : "false");
    sb_printf(&b, "isFile: %s\n", isdir ? "false" : "true");
    sb_printf(&b, "permissions: %s\n", perms);
    sb_printf(&b, "fileType: %s", isdir ? "directory" : "text");

    tool_result r = result_buf(d, &b);
    free(abs);
    return r;
}
