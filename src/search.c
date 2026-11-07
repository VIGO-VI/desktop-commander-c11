/* SPDX-License-Identifier: MIT
 * Copyright 2026 the Desktop Commander C contributors
 *
 * Streaming searches in a background thread. A search walks the tree with the
 * normal file calls, so it needs no external program.
 *
 * A session is a slot of a static array. Its rows are bytes of one block with
 * an index of 4-byte offsets, so a page costs no allocation and the memory of
 * a search is known when it starts.
 */
#include "config.h"
#include "tools.h"
#include "util.h"

#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <limits.h>
#include <pthread.h>
#include <regex.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define NAME_MAX_BYTES 256
#define ROW_CTX 0x80000000u /* bit of offs[] that marks a context row */

typedef struct {
    bool used;
    bool exited; /* the walker has left; the slot may be reused */
    char id[40];
    char root[PATH_MAX];
    char pattern[256];
    char file_pattern[256];
    bool content;
    bool literal;
    bool ignore_case;
    bool hidden;
    int64_t max_results;
    int64_t timeout_ms;
    int64_t context;
    bool early_termination;

    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t cond;

    char *rows;        /* DCM_SEARCH_BYTES of NUL-terminated rows */
    uint32_t *offs;    /* DCM_SEARCH_ROWS offsets, high bit = context */
    uint32_t nrows;
    uint32_t bytes;
    int64_t matches;
    bool done;
    bool stopped;
    bool timed_out;
    bool maxed;
    bool exact_hit;
    int64_t started_ms;
} ssess;

static pthread_mutex_t g_sessions_lock = PTHREAD_MUTEX_INITIALIZER;
static ssess g_searches[DCM_MAX_SEARCHES];
static uint64_t g_seq;

/* ---------------------------------------------------------------- helpers */

/* Adds one row. Locked: the walker and the reader share the block. */
static void row_push(ssess *s, bool ctx, const char *fmt, ...)
{
    char tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;

    pthread_mutex_lock(&s->lock);
    size_t need = (size_t)n + 1;
    if (s->nrows == DCM_SEARCH_ROWS || need > DCM_SEARCH_BYTES - s->bytes) {
        s->maxed = true;
        pthread_mutex_unlock(&s->lock);
        return;
    }
    uint32_t at = s->bytes;
    memcpy(s->rows + at, tmp, need);
    s->bytes += (uint32_t)need;
    s->offs[s->nrows++] = at | (ctx ? ROW_CTX : 0);
    if (!ctx)
        s->matches++;
    pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->lock);
}

static bool sees_stop(ssess *s)
{
    pthread_mutex_lock(&s->lock);
    bool stop = s->stopped || s->exact_hit || s->maxed;
    if (!stop && s->max_results > 0 && s->matches >= s->max_results) {
        s->maxed = true;
        stop = true;
    }
    if (!stop && s->timeout_ms > 0 && now_ms() - s->started_ms >= s->timeout_ms) {
        s->timed_out = true;
        stop = true;
    }
    pthread_mutex_unlock(&s->lock);
    return stop;
}

static bool contains_ci(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    if (n == 0)
        return true;
    for (const char *p = hay; *p; p++)
        if (strncasecmp(p, needle, n) == 0)
            return true;
    return false;
}

static bool glob_pattern(const char *p)
{
    return strpbrk(p, "*?[") != NULL;
}

static bool name_matches(const ssess *s, const char *name, const char *rel)
{
    const char *pat = s->pattern;
    if (s->literal || !glob_pattern(pat)) {
        if (s->ignore_case)
            return contains_ci(name, pat);
        return strstr(name, pat) != NULL;
    }
    int flags = s->ignore_case ? FNM_CASEFOLD : 0;
    if (fnmatch(pat, name, flags) == 0)
        return true;
    return strcmp(rel, name) != 0 && fnmatch(pat, rel, flags) == 0;
}

/* filePattern holds alternatives separated by | or , */
static bool file_pattern_ok(const ssess *s, const char *name)
{
    const char *p = s->file_pattern;
    if (!*p)
        return true;
    char alt[NAME_MAX_BYTES];
    while (*p) {
        size_t n = strcspn(p, "|,");
        while (n && *p == ' ') {
            p++;
            n--;
        }
        if (n && n < sizeof alt) {
            memcpy(alt, p, n);
            alt[n] = '\0';
            if (fnmatch(alt, name, FNM_CASEFOLD) == 0)
                return true;
        }
        p += n;
        if (*p)
            p++;
    }
    return false;
}

static void content_search_file(ssess *s, const char *path, const char *rel)
{
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
        return;
    if (st.st_size > 32 * 1024 * 1024)
        return;

    size_t len = 0;
    char *err = NULL;
    char *data = read_file_all(path, &len, &err);
    free(err);
    if (!data)
        return;
    if (data_is_binary(data, len)) {
        free(data);
        return;
    }

    regex_t re;
    bool use_re = false;
    if (!s->literal) {
        int flags = REG_EXTENDED | REG_NOSUB;
        if (s->ignore_case)
            flags |= REG_ICASE;
        use_re = regcomp(&re, s->pattern, flags) == 0;
    }

    /* Line starts, in one block: 4 bytes a line, no pointer per line. */
    uint32_t *lines = NULL;
    size_t nlines = 0, lcap = 0;
    for (char *p = data; p && *p;) {
        char *nl = strchr(p, '\n');
        if (nl)
            *nl = '\0';
        size_t l = strlen(p);
        if (l && p[l - 1] == '\r')
            p[l - 1] = '\0';
        if (nlines == lcap) {
            lcap = lcap ? lcap * 2 : 256;
            lines = xrealloc(lines, lcap * sizeof *lines);
        }
        lines[nlines++] = (uint32_t)(p - data);
        if (!nl)
            break;
        p = nl + 1;
    }

    int64_t last_shown = -1;
    for (size_t i = 0; i < nlines; i++) {
        const char *line = data + lines[i];
        bool hit;
        if (use_re)
            hit = regexec(&re, line, 0, NULL, 0) == 0;
        else if (s->ignore_case)
            hit = contains_ci(line, s->pattern);
        else
            hit = strstr(line, s->pattern) != NULL;

        if (hit) {
            if (s->context > 0) {
                int64_t from = (int64_t)i - s->context;
                if (from < 0)
                    from = 0;
                for (int64_t k = from; k < (int64_t)i; k++) {
                    if (k > last_shown)
                        row_push(s, true, "%s:%lld · %s", rel, (long long)k + 1, data + lines[k]);
                    last_shown = k;
                }
            }
            row_push(s, false, "%s:%zu - %s", rel, i + 1, line);
            last_shown = (int64_t)i;

            int64_t after_end = (int64_t)i + s->context;
            for (int64_t k = (int64_t)i + 1; k <= after_end && k < (int64_t)nlines; k++) {
                row_push(s, true, "%s:%lld · %s", rel, (long long)k + 1, data + lines[k]);
                last_shown = k;
            }
            if (sees_stop(s))
                break;
        }
    }

    if (use_re)
        regfree(&re);
    free(lines);
    free(data);
}

static void walk(ssess *s, const char *dir, const char *rel)
{
    if (sees_stop(s))
        return;

    DIR *dh = opendir(dir);
    if (!dh)
        return;

    char *names = NULL;
    size_t n = 0, cap = 0;
    struct dirent *de;
    while ((de = readdir(dh)) != NULL) {
        const char *name = de->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            continue;
        if (!s->hidden && (name[0] == '.' || strcmp(name, ".git") == 0))
            continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 32;
            names = xrealloc(names, cap * NAME_MAX_BYTES);
        }
        snprintf(names + n * NAME_MAX_BYTES, NAME_MAX_BYTES, "%s", name);
        n++;
    }
    closedir(dh);

    for (size_t i = 0; i < n && !sees_stop(s); i++) {
        const char *name = names + i * NAME_MAX_BYTES;
        char full[PATH_MAX], child_rel[PATH_MAX];
        snprintf(full, sizeof full, "%s/%s", dir, name);
        snprintf(child_rel, sizeof child_rel, "%s%s%s", *rel ? rel : "", *rel ? "/" : "", name);

        struct stat st;
        if (lstat(full, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            walk(s, full, child_rel);
            continue;
        }
        if (s->content) {
            if (file_pattern_ok(s, name))
                content_search_file(s, full, child_rel);
        } else if (name_matches(s, name, child_rel) && file_pattern_ok(s, name)) {
            row_push(s, false, "%s", child_rel);
            if (s->early_termination && str_ieq(name, s->pattern)) {
                pthread_mutex_lock(&s->lock);
                s->exact_hit = true;
                pthread_cond_broadcast(&s->cond);
                pthread_mutex_unlock(&s->lock);
                break;
            }
        }
    }

    free(names);
}

static void *search_main(void *arg)
{
    ssess *s = arg;
    struct stat st;
    if (stat(s->root, &st) == 0 && S_ISREG(st.st_mode)) {
        if (s->content)
            content_search_file(s, s->root, s->root);
        else if (name_matches(s, base_name(s->root), base_name(s->root)))
            row_push(s, false, "%s", s->root);
    } else {
        walk(s, s->root, "");
    }
    pthread_mutex_lock(&s->lock);
    s->done = true;
    s->exited = true; /* from here the slot owns nothing of this thread */
    pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->lock);
    return NULL;
}

/* Caller holds g_sessions_lock. */
static ssess *find_session(const char *id)
{
    for (size_t i = 0; i < DCM_MAX_SEARCHES; i++)
        if (g_searches[i].used && strcmp(g_searches[i].id, id) == 0)
            return &g_searches[i];
    return NULL;
}

/* A free slot, or the oldest finished one. Caller holds g_sessions_lock. */
static ssess *take_slot(void)
{
    for (size_t i = 0; i < DCM_MAX_SEARCHES; i++)
        if (!g_searches[i].used)
            return &g_searches[i];
    ssess *oldest = NULL;
    for (size_t i = 0; i < DCM_MAX_SEARCHES; i++) {
        ssess *s = &g_searches[i];
        if (s->exited && (!oldest || s->started_ms < oldest->started_ms))
            oldest = s;
    }
    return oldest;
}

tool_result tool_start_search(jdoc *d, jref a)
{
    const char *path = jstr_get(d, a, "path", NULL);
    const char *pattern = jstr_get(d, a, "pattern", NULL);
    if (!path || !*path)
        return result_error(d, "Missing path");
    if (!pattern)
        return result_error(d, "Missing pattern");
    if (strlen(pattern) >= 256)
        return result_error(d, "Pattern is longer than 255 bytes");

    char *root = NULL;
    char *err = validate_path(path, &root);
    if (err) {
        tool_result r = result_error(d, "%s", err);
        free(err);
        return r;
    }
    struct stat st;
    if (stat(root, &st) != 0) {
        tool_result r = result_error(d, "Search path does not exist: %s", path);
        free(root);
        return r;
    }

    pthread_mutex_lock(&g_sessions_lock);
    ssess *s = take_slot();
    if (s) {
        if (!s->used) {
            memset(s, 0, sizeof *s);
            pthread_mutex_init(&s->lock, NULL);
            pthread_cond_init(&s->cond, NULL);
            s->rows = xmalloc(DCM_SEARCH_BYTES);
            s->offs = xmalloc(DCM_SEARCH_ROWS * sizeof *s->offs);
            s->used = true;
        } else {
            /* Reuse a finished slot: the thread has left it. */
            pthread_mutex_lock(&s->lock);
            s->done = s->stopped = s->timed_out = s->maxed = s->exact_hit = false;
            s->nrows = s->bytes = 0;
            s->matches = 0;
            s->exited = false;
            pthread_mutex_unlock(&s->lock);
        }
        g_seq++;
        snprintf(s->id, sizeof s->id, "search-%d-%llu", (int)getpid(),
                 (unsigned long long)g_seq);
        snprintf(s->root, sizeof s->root, "%s", root);
        snprintf(s->pattern, sizeof s->pattern, "%s", pattern);
        const char *fp = jstr_get(d, a, "filePattern", "");
        snprintf(s->file_pattern, sizeof s->file_pattern, "%s", fp ? fp : "");
        s->content = strcmp(jstr_get(d, a, "searchType", "files"), "content") == 0;
        s->literal = jbool_get(d, a, "literalSearch", false);
        s->ignore_case = jbool_get(d, a, "ignoreCase", true);
        s->hidden = jbool_get(d, a, "includeHidden", false);
        s->early_termination = jbool_get(d, a, "earlyTermination", !s->content);
        s->max_results = jint_get(d, a, "maxResults", 0);
        s->timeout_ms = jint_get(d, a, "timeout_ms", 0);
        s->context = jint_get(d, a, "contextLines", 5);
        if (s->context < 0)
            s->context = 0;
        s->started_ms = now_ms();
    }
    pthread_mutex_unlock(&g_sessions_lock);
    free(root);

    if (!s)
        return result_error(d, "No free search session: stop one with stop_search.");

    if (pthread_create(&s->thread, NULL, search_main, s) != 0)
        return result_error(d, "Cannot start the search thread");
    pthread_detach(s->thread);

    /* A short head start, so small folders answer with their results at once. */
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_nsec += 150 * 1000000L;
    if (until.tv_nsec >= 1000000000L) {
        until.tv_sec++;
        until.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&s->lock);
    while (!s->done)
        if (pthread_cond_timedwait(&s->cond, &s->lock, &until) == ETIMEDOUT)
            break;
    size_t shown = s->nrows < 100 ? s->nrows : 100;
    bool done = s->done;
    int64_t matches = s->matches;

    sbuf b;
    sb_init(&b);
    sb_printf(&b, "Started %s search session: %s\n", s->content ? "content" : "file", s->id);
    sb_printf(&b, "Pattern: \"%s\"\nPath: %s\n", pattern, path);
    sb_printf(&b, "Status: %s\nRuntime: %lldms\n", done ? "COMPLETED" : "RUNNING",
              (long long)(now_ms() - s->started_ms));
    sb_printf(&b, "Total results: %lld %s\n", (long long)matches,
              matches == 1 ? "match" : "matches");
    if (shown > 0) {
        sb_put(&b, "\nInitial results:\n");
        for (size_t i = 0; i < shown; i++)
            sb_printf(&b, "%s\n", s->rows + (s->offs[i] & ~ROW_CTX));
    }
    pthread_mutex_unlock(&s->lock);
    if (!done)
        sb_put(&b, "\nSearch in progress. Use get_more_search_results to get more.");
    else
        sb_put(&b, "\nSearch completed.");

    tool_result r = result_buf(d, &b);
    return r;
}

tool_result tool_get_more_search_results(jdoc *d, jref a)
{
    const char *id = jstr_get(d, a, "sessionId", NULL);
    if (!id)
        return result_error(d, "Missing sessionId");
    int64_t offset = jint_get(d, a, "offset", 0);
    int64_t length = jint_get(d, a, "length", 100);
    if (offset < 0)
        offset = 0;
    if (length < 1)
        length = 100;

    pthread_mutex_lock(&g_sessions_lock);
    ssess *s = find_session(id);
    pthread_mutex_unlock(&g_sessions_lock);
    if (!s)
        return result_error(d, "Search session %s not found", id);

    pthread_mutex_lock(&s->lock);
    if (!s->done) {
        /* Give a running search a moment to fill this page. */
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_nsec += 500 * 1000000L;
        if (until.tv_nsec >= 1000000000L) {
            until.tv_sec++;
            until.tv_nsec -= 1000000000L;
        }
        size_t want = (size_t)(offset + length);
        while (!s->done && s->nrows < want)
            if (pthread_cond_timedwait(&s->cond, &s->lock, &until) == ETIMEDOUT)
                break;
    }
    size_t total = s->nrows;
    size_t from = (size_t)offset < total ? (size_t)offset : total;
    size_t count = (size_t)length;
    if (from + count > total)
        count = total - from;
    bool done = s->done;
    bool stopped = s->stopped, timed_out = s->timed_out, maxed = s->maxed;
    bool exact_hit = s->exact_hit;
    int64_t matches = s->matches;
    int64_t timeout = s->timeout_ms;

    sbuf b;
    sb_init(&b);
    if (count == 0) {
        if (done)
            sb_printf(&b, "No results at offset %lld (%zu in total).", (long long)offset, total);
        else
            sb_printf(&b, "Still running: %zu rows so far, none at offset %lld yet.", total,
                      (long long)offset);
    } else {
        sb_printf(&b, "[Showing %zu rows from %zu (total: %zu rows, %zu remaining)]\n\n",
                  count, from, total, total - (from + count));
        for (size_t i = from; i < from + count; i++)
            sb_printf(&b, "%s\n", s->rows + (s->offs[i] & ~ROW_CTX));
        if (from + count < total)
            sb_printf(&b, "\nMore results available. Use get_more_search_results with offset: %zu",
                      from + count);
        else if (!done)
            sb_put(&b, "\nStill running: more may come.");
    }
    if (done) {
        if (maxed)
            sb_printf(&b, "\nStopped at maxResults (%lld): there may be more.",
                      (long long)matches);
        else if (timed_out)
            sb_printf(&b, "\nStopped after %lld ms: results may be incomplete.",
                      (long long)timeout);
        else if (stopped)
            sb_put(&b, "\nStopped on request: results may be incomplete.");
        else if (exact_hit)
            sb_put(&b, "\nStopped early: exact filename match.");
        else
            sb_put(&b, "\nSearch completed.");
    }
    pthread_mutex_unlock(&s->lock);

    tool_result r = result_buf(d, &b);
    return r;
}

tool_result tool_stop_search(jdoc *d, jref a)
{
    const char *id = jstr_get(d, a, "sessionId", NULL);
    if (!id)
        return result_error(d, "Missing sessionId");
    pthread_mutex_lock(&g_sessions_lock);
    ssess *s = find_session(id);
    pthread_mutex_unlock(&g_sessions_lock);
    if (!s) {
        sbuf b;
        sb_init(&b);
        sb_printf(&b, "Search session %s not found.", id);
        tool_result r = result_buf(d, &b);
        return r;
    }

    pthread_mutex_lock(&s->lock);
    bool done = s->done;
    s->stopped = true;
    pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->lock);

    sbuf b;
    sb_init(&b);
    sb_printf(&b, "Search session %s %s.", id,
              done ? "already completed" : "terminated successfully");
    tool_result r = result_buf(d, &b);
    return r;
}

tool_result tool_list_searches(jdoc *d, jref a)
{
    (void)a;
    sbuf b;
    sb_init(&b);
    size_t count = 0;

    pthread_mutex_lock(&g_sessions_lock);
    for (size_t i = 0; i < DCM_MAX_SEARCHES; i++)
        count += g_searches[i].used;
    if (count == 0) {
        pthread_mutex_unlock(&g_sessions_lock);
        sb_free(&b);
        return result_text(d, "No active searches.");
    }
    sb_printf(&b, "Searches (%zu):\n\n", count);
    for (size_t i = 0; i < DCM_MAX_SEARCHES; i++) {
        ssess *s = &g_searches[i];
        if (!s->used)
            continue;
        pthread_mutex_lock(&s->lock);
        sb_printf(&b, "Session: %s\n  Type: %s\n  Pattern: \"%s\"\n  Status: %s\n"
                      "  Runtime: %llds\n  Results: %lld\n\n",
                  s->id, s->content ? "content" : "files", s->pattern,
                  s->done ? "COMPLETED" : "RUNNING",
                  (long long)((now_ms() - s->started_ms) / 1000), (long long)s->matches);
        pthread_mutex_unlock(&s->lock);
    }
    pthread_mutex_unlock(&g_sessions_lock);

    tool_result r = result_buf(d, &b);
    return r;
}
