/* SPDX-License-Identifier: MIT
 * Copyright 2026 the Desktop Commander C contributors
 *
 * Filesystem tools: read_file, read_multiple_files, write_file,
 * create_directory, list_directory, move_file, get_file_info.
 */
#include "tools.h"
#include "util.h"

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

tool_result result_text(const char *text)
{
    tool_result r = {0};
    r.content = jarr();
    json *block = jobj();
    jobj_set(block, "type", jstr("text"));
    jobj_set(block, "text", jstr(text ? text : ""));
    jarr_push(r.content, block);
    return r;
}

tool_result result_error(const char *fmt, ...)
{
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int need = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    char *msg = xmalloc((size_t)(need > 0 ? need : 0) + 1);
    if (need > 0)
        vsnprintf(msg, (size_t)need + 1, fmt, ap2);
    else
        msg[0] = '\0';
    va_end(ap2);
    tool_result r = result_text(msg);
    free(msg);
    r.is_error = true;
    return r;
}

tool_result result_text_struct(const char *text, json *structured)
{
    tool_result r = result_text(text);
    r.structured = structured;
    return r;
}

void result_free(tool_result *r)
{
    if (!r)
        return;
    json_free(r->content);
    json_free(r->structured);
    r->content = NULL;
    r->structured = NULL;
}

/* ---------------------------------------------------------------- base64 */

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char *base64_encode(const unsigned char *data, size_t len)
{
    size_t out_len = ((len + 2) / 3) * 4;
    char *out = xmalloc(out_len + 1);
    size_t j = 0;
    for (size_t i = 0; i < len; i += 3) {
        unsigned v = (unsigned)data[i] << 16;
        if (i + 1 < len)
            v |= (unsigned)data[i + 1] << 8;
        if (i + 2 < len)
            v |= data[i + 2];
        out[j++] = B64[(v >> 18) & 0x3F];
        out[j++] = B64[(v >> 12) & 0x3F];
        out[j++] = (i + 1 < len) ? B64[(v >> 6) & 0x3F] : '=';
        out[j++] = (i + 2 < len) ? B64[v & 0x3F] : '=';
    }
    out[j] = '\0';
    return out;
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
static char *slice_lines(const char *text, long long offset, long long length,
                         long long total_lines, long long *count_out,
                         bool *tail_out)
{
    *tail_out = offset < 0;
    *count_out = 0;
    if (!text)
        return xstrdup("");

    if (offset < 0) {
        /* Tail: the last |offset| lines. */
        long long want = -offset;
        if (want > total_lines)
            want = total_lines;
        long long start = total_lines - want;
        size_t skip = start > 0 ? bytes_for_lines(text, (size_t)start) : 0;
        *count_out = want;
        return join_lines_clean(text + skip, strlen(text + skip));
    }

    if (length < 0)
        length = 0;
    size_t start = offset > 0 ? bytes_for_lines(text, (size_t)offset) : 0;
    if (start >= strlen(text)) {
        *count_out = 0;
        return xstrdup("");
    }

    const char *p = text + start;
    long long n = 0;
    size_t bytes = 0;
    while (*p && n < length) {
        const char *nl = strchr(p, '\n');
        if (!nl) {
            bytes = strlen(p);
            p += bytes;
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

tool_result tool_read_file(json *a)
{
    const char *path = jstr_get(a, "path", NULL);
    if (!path || !*path)
        return result_error("Missing path");

    if (jbool_get(a, "isUrl", false))
        return result_error("URL reads are not supported in this build; fetch with start_process (curl/wget).");

    config *cfg = config_load();
    long long length = jint_get(a, "length", cfg->read_line_limit);
    long long offset = jint_get(a, "offset", 0);
    if (jget(a, "length") == NULL)
        length = cfg->read_line_limit;
    config_free(cfg);

    char *abs = NULL;
    char *err = validate_path(path, &abs);
    if (err) {
        tool_result r = result_error("%s", err);
        free(err);
        return r;
    }

    struct stat st;
    if (stat(abs, &st) != 0) {
        tool_result r = result_error("Cannot read %s: %s", path, strerror(errno));
        free(abs);
        return r;
    }

    if (S_ISDIR(st.st_mode)) {
        /* Same fallback as the original: a listing with a hint. */
        json *args = jobj();
        jobj_set(args, "path", jstr(path));
        tool_result listed = tool_list_directory(args);
        json_free(args);
        sbuf b;
        sb_init(&b);
        sb_put(&b, "This is a directory, not a file. Use list_directory instead.\n\n");
        json *first = listed.content->u.a.v[0];
        sb_put(&b, jstr_get(first, "text", ""));
        result_free(&listed);
        tool_result r = result_text(b.p);
        sb_free(&b);
        free(abs);
        return r;
    }

    size_t len = 0;
    char *data = read_file_all(abs, &len, &err);
    if (!data) {
        tool_result r = result_error("%s", err);
        free(err);
        free(abs);
        return r;
    }

    /* Images travel as an image block so the model can see them. */
    if (file_is_image(abs)) {
        char *b64 = base64_encode((const unsigned char *)data, len);
        json *content = jarr();
        json *summary = jobj();
        jobj_set(summary, "type", jstr("text"));
        char *sum = xasprintf("Image file: %s (%s)", path, mime_type_for(abs));
        jobj_set(summary, "text", jstr(sum));
        free(sum);
        jarr_push(content, summary);
        json *img = jobj();
        jobj_set(img, "type", jstr("image"));
        jobj_set(img, "data", jstr(b64));
        jobj_set(img, "mimeType", jstr(mime_type_for(abs)));
        jarr_push(content, img);
        free(b64);
        free(data);
        free(abs);
        tool_result r = {0};
        r.content = content;
        return r;
    }

    if (data_is_binary(data, len)) {
        free(data);
        free(abs);
        return result_error("Cannot read %s as text: the file is binary", path);
    }

    long long total = (long long)count_lines(data);
    long long count = 0;
    bool tail = false;
    char *slice = slice_lines(data, offset, length, total, &count, &tail);
    free(data);

    sbuf b;
    sb_init(&b);
    long long remaining;
    if (tail) {
        sb_printf(&b, "[Reading last %lld lines (total: %lld lines)]\n\n", count, total);
    } else {
        remaining = total - (offset + count);
        if (remaining < 0)
            remaining = 0;
        if (offset == 0)
            sb_printf(&b, "[Reading %lld lines from start (total: %lld lines, %lld remaining)]\n\n",
                      count, total, remaining);
        else
            sb_printf(&b, "[Reading %lld lines from line %lld (total: %lld lines, %lld remaining)]\n\n",
                      count, offset, total, remaining);
    }
    sb_put(&b, slice);

    tool_result r = result_text(b.p);
    sb_free(&b);
    free(slice);
    free(abs);
    return r;
}

tool_result tool_read_multiple_files(json *a)
{
    json *paths = jget(a, "paths");
    if (!paths || paths->t != J_ARR || paths->u.a.n == 0)
        return result_error("paths must be a non-empty array of strings");

    sbuf summary;
    sb_init(&summary);
    json *content = jarr();

    for (size_t i = 0; i < paths->u.a.n; i++) {
        const char *path = paths->u.a.v[i]->t == J_STR ? paths->u.a.v[i]->u.s : "";
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
            json *header = jobj();
            jobj_set(header, "type", jstr("text"));
            char *h = xasprintf("\n--- %s contents: ---\n", path);
            jobj_set(header, "text", jstr(h));
            free(h);
            jarr_push(content, header);
            char *b64 = base64_encode((const unsigned char *)data, len);
            json *img = jobj();
            jobj_set(img, "type", jstr("image"));
            jobj_set(img, "data", jstr(b64));
            jobj_set(img, "mimeType", jstr(mime_type_for(abs)));
            jarr_push(content, img);
            free(b64);
        } else {
            sb_printf(&summary, "%s: %s (text)", path, mime_type_for(abs));
            char *body = xasprintf("\n--- %s contents: ---\n%s", path, data);
            json *block = jobj();
            jobj_set(block, "type", jstr("text"));
            jobj_set(block, "text", jstr(body));
            jarr_push(content, block);
            free(body);
        }
        free(data);
        free(abs);
    }

    json *head = jobj();
    jobj_set(head, "type", jstr("text"));
    char *s = sb_take(&summary);
    jobj_set(head, "text", jstr(s));
    free(s);
    /* The summary block goes first, like the original. */
    json *all = jarr();
    jarr_push(all, head);
    for (size_t i = 0; i < content->u.a.n; i++) {
        jarr_push(all, content->u.a.v[i]);
        content->u.a.v[i] = NULL;
    }
    content->u.a.n = 0;
    json_free(content);

    tool_result r = {0};
    r.content = all;
    return r;
}

/* ---------------------------------------------------------------- writing */

tool_result tool_write_file(json *a)
{
    const char *path = jstr_get(a, "path", NULL);
    if (!path || !*path)
        return result_error("Missing path");
    json *cv = jget(a, "content");
    if (!cv || cv->t != J_STR)
        return result_error("content must be a string");

    bool mode_given = jget(a, "mode") != NULL;
    const char *mode = jstr_get(a, "mode", "rewrite");
    bool append = strcmp(mode, "append") == 0;
    if (!append && strcmp(mode, "rewrite") != 0)
        return result_error("mode must be 'rewrite' or 'append'");

    char *abs = NULL;
    char *err = validate_path(path, &abs);
    if (err) {
        tool_result r = result_error("%s", err);
        free(err);
        return r;
    }

    if (!mode_given) {
        struct stat st;
        if (stat(abs, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0) {
            tool_result r = result_error(
                "Write rejected: %s already has content (%lld bytes) and no mode was given. "
                "Retry with mode 'append' or mode 'rewrite'.",
                path, (long long)st.st_size);
            free(abs);
            return r;
        }
    }

    unsigned long long lines = count_lines(cv->u.s);
    char *out = write_file_atomic(abs, cv->u.s, strlen(cv->u.s), append, 0644, &err);
    if (!out) {
        tool_result r = result_error("%s", err);
        free(err);
        free(abs);
        return r;
    }
    free(out);

    char *msg = xasprintf("Successfully %s %s (%llu lines)",
                          append ? "appended to" : "wrote to", path, lines);
    tool_result r = result_text(msg);
    free(msg);
    free(abs);
    return r;
}

tool_result tool_create_directory(json *a)
{
    const char *path = jstr_get(a, "path", NULL);
    if (!path || !*path)
        return result_error("Missing path");

    char *abs = NULL;
    char *err = validate_path(path, &abs);
    if (err) {
        tool_result r = result_error("%s", err);
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
        tool_result r = result_error("Cannot create %s: %s", path, strerror(errno));
        free(tmp);
        free(abs);
        return r;
    }
    free(tmp);
    free(abs);
    char *msg = xasprintf("Successfully created directory %s", path);
    tool_result r = result_text(msg);
    free(msg);
    return r;
}

tool_result tool_move_file(json *a)
{
    const char *src = jstr_get(a, "source", NULL);
    const char *dst = jstr_get(a, "destination", NULL);
    if (!src || !dst)
        return result_error("source and destination are required");

    char *asrc = NULL, *adst = NULL;
    char *err = validate_path(src, &asrc);
    if (!err)
        err = validate_path(dst, &adst);
    if (err) {
        tool_result r = result_error("%s", err);
        free(err);
        free(asrc);
        return r;
    }
    if (rename(asrc, adst) != 0) {
        tool_result r = result_error("Cannot move %s to %s: %s", src, dst, strerror(errno));
        free(asrc);
        free(adst);
        return r;
    }
    free(asrc);
    free(adst);
    char *msg = xasprintf("Successfully moved %s to %s", src, dst);
    tool_result r = result_text(msg);
    free(msg);
    return r;
}

/* ---------------------------------------------------------------- listing */

static int cmp_names(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static void list_recursive(json *out, const char *dir, int depth, const char *rel,
                           bool top, char **errbuf)
{
    if (depth <= 0)
        return;
    DIR *d = opendir(dir);
    if (!d) {
        char *msg;
        if (errno == ENOENT)
            msg = xasprintf("[NOT_FOUND] %s — path does not exist", rel && *rel ? rel : base_name(dir));
        else
            msg = xasprintf("[DENIED] %s — not accessible", rel && *rel ? rel : base_name(dir));
        jarr_push(out, jstr(msg));
        free(msg);
        return;
    }

    char **names = NULL;
    size_t n = 0, cap = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 32;
            names = xrealloc(names, cap * sizeof *names);
        }
        names[n++] = xstrdup(de->d_name);
    }
    closedir(d);

    if (n)
        qsort(names, n, sizeof *names, cmp_names);

    size_t show = n;
    size_t hidden = 0;
    if (!top && n > 100) {
        show = 100;
        hidden = n - 100;
    }

    for (size_t i = 0; i < show; i++) {
        char *full = xasprintf("%s/%s", dir, names[i]);
        char *display = rel && *rel ? xasprintf("%s/%s", rel, names[i]) : xstrdup(names[i]);
        struct stat st;
        bool isdir = false;
        if (lstat(full, &st) == 0) {
            if (S_ISDIR(st.st_mode))
                isdir = true;
            else if (S_ISLNK(st.st_mode)) {
                struct stat target;
                if (stat(full, &target) == 0 && S_ISDIR(target.st_mode))
                    isdir = true;
            }
        }
        char *line = xasprintf("%s %s", isdir ? "[DIR]" : "[FILE]", display);
        jarr_push(out, jstr(line));
        free(line);

        if (isdir && depth > 1) {
            char *checked = NULL;
            char *verr = validate_path(full, &checked);
            if (!verr) {
                list_recursive(out, checked, depth - 1, display, false, errbuf);
                free(checked);
            }
            free(verr);
        }
        free(display);
        free(full);
    }

    if (hidden) {
        const char *dp = rel && *rel ? rel : base_name(dir);
        char *warn = xasprintf("[WARNING] %s: %zu items hidden (showing first 100 of %zu total)",
                               dp, hidden, n);
        jarr_push(out, jstr(warn));
        free(warn);
    }

    for (size_t i = 0; i < n; i++)
        free(names[i]);
    free(names);
    (void)errbuf;
}

tool_result tool_list_directory(json *a)
{
    const char *path = jstr_get(a, "path", NULL);
    if (!path || !*path)
        return result_error("Missing path");
    long long depth = jint_get(a, "depth", 2);
    if (depth < 1)
        depth = 1;

    char *abs = NULL;
    char *err = validate_path(path, &abs);
    if (err) {
        tool_result r = result_error("%s", err);
        free(err);
        return r;
    }

    json *lines = jarr();
    struct stat st;
    if (stat(abs, &st) == 0 && S_ISREG(st.st_mode)) {
        char *line = xasprintf("[FILE] %s", base_name(abs));
        jarr_push(lines, jstr(line));
        free(line);
    } else {
        char *eb = NULL;
        list_recursive(lines, abs, (int)depth, "", true, &eb);
        free(eb);
    }

    sbuf b;
    sb_init(&b);
    for (size_t i = 0; i < lines->u.a.n; i++) {
        if (i)
            sb_putc(&b, '\n');
        sb_put(&b, lines->u.a.v[i]->u.s);
    }
    json_free(lines);
    tool_result r = result_text(b.p);
    sb_free(&b);
    free(abs);
    return r;
}

/* ---------------------------------------------------------------- info */

static char *iso_time(time_t t)
{
    struct tm tm;
    char buf[64];
    if (gmtime_r(&t, &tm) == NULL)
        return xstrdup("unknown");
    strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S.000Z", &tm);
    return xstrdup(buf);
}

tool_result tool_get_file_info(json *a)
{
    const char *path = jstr_get(a, "path", NULL);
    if (!path || !*path)
        return result_error("Missing path");

    char *abs = NULL;
    char *err = validate_path(path, &abs);
    if (err) {
        tool_result r = result_error("%s", err);
        free(err);
        return r;
    }
    struct stat st;
    if (stat(abs, &st) != 0) {
        tool_result r = result_error("Cannot stat %s: %s", path, strerror(errno));
        free(abs);
        return r;
    }

#ifdef __APPLE__
    char *created = iso_time(st.st_birthtime);
#else
    char *created = iso_time(st.st_ctime);
#endif
    char *modified = iso_time(st.st_mtime);
    char *accessed = iso_time(st.st_atime);
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

    tool_result r = result_text(b.p);
    sb_free(&b);
    free(created);
    free(modified);
    free(accessed);
    free(abs);
    return r;
}
