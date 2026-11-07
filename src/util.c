/* SPDX-License-Identifier: MIT
 * Copyright 2026 the Desktop Commander C contributors
 */
#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) {
        fputs("dcm: out of memory\n", stderr);
        exit(2);
    }
    return p;
}

void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) {
        fputs("dcm: out of memory\n", stderr);
        exit(2);
    }
    return q;
}

char *xstrdup(const char *s)
{
    size_t n = strlen(s ? s : "") + 1;
    char *p = xmalloc(n);
    if (s)
        memcpy(p, s, n);
    else
        p[0] = '\0';
    return p;
}

char *xasprintf(const char *fmt, ...)
{
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int need = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (need < 0) {
        va_end(ap2);
        return xstrdup("");
    }
    char *p = xmalloc((size_t)need + 1);
    vsnprintf(p, (size_t)need + 1, fmt, ap2);
    va_end(ap2);
    return p;
}

const char *dcm_home_dir(void)
{
    const char *h = getenv("HOME");
    return h && *h ? h : "/";
}

const char *dcm_config_dir(void)
{
    static char *dir;
    if (dir)
        return dir;
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg)
        dir = xasprintf("%s/desktop-commander", xdg);
    else
        dir = xasprintf("%s/.config/desktop-commander", dcm_home_dir());
    return dir;
}

long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---------------------------------------------------------------- paths */

char *path_expand(const char *path)
{
    if (!path || !*path)
        return xstrdup("");
    if (path[0] == '~' && (path[1] == '/' || path[1] == '\0'))
        return xasprintf("%s%s", dcm_home_dir(), path + 1);
    return xstrdup(path);
}

char *path_absolute(const char *path)
{
    char *expanded = path_expand(path);
    char *abs;
    if (expanded[0] == '/') {
        abs = expanded;
    } else {
        char cwd[PATH_MAX];
        if (!getcwd(cwd, sizeof cwd))
            cwd[0] = '\0';
        abs = xasprintf("%s/%s", cwd, expanded);
        free(expanded);
    }
    /* Collapse . and .. where the target exists. */
    char *real = realpath(abs, NULL);
    if (real) {
        free(abs);
        return real;
    }
    /* Missing leaf: resolve the parent and keep the name. */
    char *slash = strrchr(abs, '/');
    if (slash && slash != abs) {
        *slash = '\0';
        char *parent = realpath(abs, NULL);
        if (parent) {
            char *out = xasprintf("%s/%s", parent, slash + 1);
            free(parent);
            free(abs);
            return out;
        }
        *slash = '/';
    }
    return abs;
}

static bool path_under(const char *path, const char *root)
{
    size_t n = strlen(root);
    if (n == 0)
        return false;
    if (strcmp(root, "/") == 0)
        return true;
    if (strncmp(path, root, n) != 0)
        return false;
    return path[n] == '\0' || path[n] == '/';
}

char *validate_path(const char *path, char **out)
{
    *out = NULL;
    if (!path || !*path)
        return xstrdup("Path is empty");

    char *abs = path_absolute(path);
    config *cfg = config_load();

    /* A missing path cannot be symlink-checked; realpath of the parent is
     * inside path_absolute, so an escaping symlink still shows up here. */
    char *real = realpath(abs, NULL);
    char *check = real ? real : abs;

    if (cfg->nallowed > 0) {
        bool ok = false;
        for (size_t i = 0; i < cfg->nallowed; i++) {
            char *root = realpath(cfg->allowed[i], NULL);
            if (!root)
                continue;
            if (path_under(check, root))
                ok = true;
            free(root);
            if (ok)
                break;
        }
        if (!ok) {
            config_free(cfg);
            char *msg = xasprintf("Access denied: path is outside allowedDirectories: %s", path);
            free(real);
            free(abs);
            return msg;
        }
    }

    config_free(cfg);
    free(real);
    *out = abs;
    return NULL;
}

/* ---------------------------------------------------------------- files */

char *read_file_all(const char *path, size_t *len, char **err)
{
    *err = NULL;
    FILE *f = fopen(path, "rb");
    if (!f) {
        *err = xasprintf("Cannot read %s: %s", path, strerror(errno));
        return NULL;
    }
    sbuf b;
    sb_init(&b);
    char tmp[65536];
    size_t n;
    while ((n = fread(tmp, 1, sizeof tmp, f)) > 0)
        sb_putn(&b, tmp, n);
    if (ferror(f)) {
        *err = xasprintf("Cannot read %s: %s", path, strerror(errno));
        fclose(f);
        sb_free(&b);
        return NULL;
    }
    fclose(f);
    if (len)
        *len = b.n;
    return sb_take(&b);
}

char *write_file_atomic(const char *path, const char *data, size_t len, bool append,
                        mode_t mode, char **err)
{
    *err = NULL;
    if (append) {
        FILE *f = fopen(path, "ab");
        if (!f) {
            *err = xasprintf("Cannot write %s: %s", path, strerror(errno));
            return NULL;
        }
        if (len && fwrite(data, 1, len, f) != len) {
            *err = xasprintf("Cannot write %s: %s", path, strerror(errno));
            fclose(f);
            return NULL;
        }
        fclose(f);
        return xstrdup(path);
    }

    char *dir = dir_name(path);
    char *tmpl = xasprintf("%s/.dcm-XXXXXX", dir);
    int fd = mkstemp(tmpl);
    if (fd < 0) {
        *err = xasprintf("Cannot write %s: %s", path, strerror(errno));
        free(tmpl);
        free(dir);
        return NULL;
    }
    (void)fchmod(fd, mode);

    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, data + off, len - off);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            *err = xasprintf("Cannot write %s: %s", path, strerror(errno));
            close(fd);
            unlink(tmpl);
            free(tmpl);
            free(dir);
            return NULL;
        }
        off += (size_t)w;
    }
    if (close(fd) != 0) {
        *err = xasprintf("Cannot write %s: %s", path, strerror(errno));
        unlink(tmpl);
        free(tmpl);
        free(dir);
        return NULL;
    }
    if (rename(tmpl, path) != 0) {
        *err = xasprintf("Cannot replace %s: %s", path, strerror(errno));
        unlink(tmpl);
        free(tmpl);
        free(dir);
        return NULL;
    }
    free(tmpl);
    free(dir);
    return xstrdup(path);
}

/* ---------------------------------------------------------------- types */

const char *mime_type_for(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot)
        return "text/plain";
    if (str_ieq(dot, ".png")) return "image/png";
    if (str_ieq(dot, ".jpg") || str_ieq(dot, ".jpeg")) return "image/jpeg";
    if (str_ieq(dot, ".gif")) return "image/gif";
    if (str_ieq(dot, ".webp")) return "image/webp";
    if (str_ieq(dot, ".svg")) return "image/svg+xml";
    if (str_ieq(dot, ".pdf")) return "application/pdf";
    if (str_ieq(dot, ".json")) return "application/json";
    if (str_ieq(dot, ".xml")) return "application/xml";
    return "text/plain";
}

bool file_is_image(const char *path)
{
    const char *m = mime_type_for(path);
    return strncmp(m, "image/", 6) == 0 && !str_ieq(strrchr(path, '.') ? strrchr(path, '.') : "", ".svg");
}

bool data_is_binary(const char *data, size_t len)
{
    size_t check = len < 8000 ? len : 8000;
    for (size_t i = 0; i < check; i++) {
        unsigned char c = (unsigned char)data[i];
        if (c == 0)
            return true;
    }
    return false;
}

/* ---------------------------------------------------------------- text */

char *str_replace_all(const char *hay, const char *from, const char *to,
                      long long expected, long long *count_out)
{
    size_t flen = strlen(from);
    size_t tlen = strlen(to);
    long long count = 0;
    for (const char *p = hay; (p = strstr(p, from)) != NULL; p += flen)
        count++;
    if (count_out)
        *count_out = count;
    if (expected >= 0 && count != expected)
        return NULL;
    if (flen == 0)
        return xstrdup(hay);

    sbuf b;
    sb_init(&b);
    const char *p = hay;
    const char *hit;
    while ((hit = strstr(p, from)) != NULL) {
        sb_putn(&b, p, (size_t)(hit - p));
        sb_putn(&b, to, tlen);
        p = hit + flen;
    }
    sb_put(&b, p);
    return sb_take(&b);
}

size_t count_lines(const char *s)
{
    if (!s || !*s)
        return 0;
    size_t n = 0;
    for (const char *p = s; *p; p++)
        if (*p == '\n')
            n++;
    if (s[strlen(s) - 1] != '\n')
        n++;
    return n;
}

size_t bytes_for_lines(const char *s, size_t n)
{
    size_t seen = 0;
    for (size_t i = 0; s[i]; i++) {
        if (s[i] == '\n') {
            if (++seen == n)
                return i + 1;
        }
    }
    return strlen(s);
}

size_t count_lines_upto(const char *s, size_t upto)
{
    size_t n = 0;
    for (size_t i = 0; i < upto && s[i]; i++)
        if (s[i] == '\n')
            n++;
    return n;
}

const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

char *dir_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    if (!slash)
        return xstrdup(".");
    if (slash == path)
        return xstrdup("/");
    char *out = xmalloc((size_t)(slash - path) + 1);
    memcpy(out, path, (size_t)(slash - path));
    out[slash - path] = '\0';
    return out;
}

bool str_ieq(const char *a, const char *b)
{
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return false;
        a++;
        b++;
    }
    return *a == *b;
}

bool str_iends(const char *s, const char *suffix)
{
    size_t n = strlen(s), m = strlen(suffix);
    if (m > n)
        return false;
    return str_ieq(s + n - m, suffix);
}

bool str_starts(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

/* ---------------------------------------------------------------- blocklist */

/* Split a command line at unquoted ; && || | & and inside $( ) and ` `.
 * Each element is the first word of one command. */
char **command_split(const char *cmd, size_t *n_out)
{
    char **out = NULL;
    size_t n = 0, cap = 0;

    sbuf cur;
    sb_init(&cur);
    bool in_single = false, in_double = false, escaped = false;

    size_t len = strlen(cmd);
    for (size_t i = 0; i <= len; i++) {
        char c = i < len ? cmd[i] : '\0';
        bool sep = false;
        if (!in_single && !in_double && !escaped) {
            if (c == '\0' || c == ';' || c == '|' || c == '&' || c == '`' ||
                c == '(' || c == ')')
                sep = true;
        }
        if (escaped) {
            escaped = false;
            sb_putc(&cur, c);
            continue;
        }
        if (c == '\\' && !in_single) {
            escaped = true;
            sb_putc(&cur, c);
            continue;
        }
        if (c == '\'' && !in_double) {
            in_single = !in_single;
            sb_putc(&cur, c);
            continue;
        }
        if (c == '"' && !in_single) {
            in_double = !in_double;
            sb_putc(&cur, c);
            continue;
        }
        if (sep) {
            if (cur.n) {
                char *s = sb_take(&cur);
                if (n == cap) {
                    cap = cap ? cap * 2 : 8;
                    out = xrealloc(out, cap * sizeof *out);
                }
                out[n++] = s;
            }
            if (c == '\0')
                break;
            continue;
        }
        sb_putc(&cur, c);
    }
    sb_free(&cur);

    /* Keep the first word of each command, unquoted, lowercased. The word
     * moves to the base of its buffer so it can be freed by that base. */
    for (size_t i = 0; i < n; i++) {
        char *s = out[i];
        char *w = s;
        while (*w == ' ' || *w == '\t')
            w++;
        size_t wlen = 0;
        while (w[wlen] && w[wlen] != ' ' && w[wlen] != '\t')
            wlen++;
        /* Surrounding quotes belong to the shell, not the name. */
        if (wlen >= 2 && ((w[0] == '"' && w[wlen - 1] == '"') ||
                          (w[0] == '\'' && w[wlen - 1] == '\''))) {
            w++;
            wlen -= 2;
        }
        memmove(s, w, wlen);
        s[wlen] = '\0';
        for (char *p = s; *p; p++)
            *p = (char)tolower((unsigned char)*p);
    }
    *n_out = n;
    return out;
}

bool command_is_blocked(const config *c, const char *cmd)
{
    size_t n = 0;
    char **cmds = command_split(cmd, &n);
    bool blocked = false;
    for (size_t i = 0; i < n && !blocked; i++)
        for (size_t k = 0; k < c->nblocked; k++)
            if (strcmp(cmds[i], c->blocked[k]) == 0) {
                blocked = true;
                break;
            }
    for (size_t i = 0; i < n; i++)
        free(cmds[i]);
    free(cmds);
    return blocked;
}
