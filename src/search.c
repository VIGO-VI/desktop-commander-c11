/* SPDX-License-Identifier: MIT
 * Copyright 2026 the Desktop Commander C contributors
 *
 * Streaming searches in a background thread. A search walks the tree with the
 * normal file calls, so it needs no external program.
 */
#include "tools.h"
#include "util.h"

#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <pthread.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MAX_SEARCH_RESULTS 100000
#define MAX_LINE_BYTES (8u * 1024 * 1024)

typedef struct {
    char *row;     /* formatted result line */
    bool context;  /* a line shown around a match */
} srow;

typedef struct ssess {
    char id[48];
    char *root;
    char *pattern;
    bool content;      /* searchType == content */
    bool literal;
    bool ignore_case;
    bool hidden;
    char *file_pattern;
    long long max_results;
    long long timeout_ms;
    long long context;
    bool early_termination;

    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    srow *rows;
    size_t n, cap;
    long long matches;
    bool done;
    bool stopped;
    bool timed_out;
    bool maxed;
    bool exact_hit;
    long long started_ms;
    struct ssess *next;
} ssess;

static pthread_mutex_t g_sessions_lock = PTHREAD_MUTEX_INITIALIZER;
static ssess *g_sessions;
static unsigned long g_seq;

/* ---------------------------------------------------------------- helpers */

static void row_push(ssess *s, char *row, bool context)
{
    pthread_mutex_lock(&s->lock);
    if (s->n == s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 64;
        s->rows = xrealloc(s->rows, cap * sizeof *s->rows);
        s->cap = cap;
    }
    s->rows[s->n].row = row;
    s->rows[s->n].context = context;
    s->n++;
    if (!context)
        s->matches++;
    pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->lock);
}

static bool sees_stop(ssess *s)
{
    pthread_mutex_lock(&s->lock);
    bool stop = s->stopped || s->exact_hit;
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

static bool file_hidden(const char *name)
{
    return name[0] == '.';
}

static bool glob_pattern(const char *p)
{
    return strpbrk(p, "*?[") != NULL;
}

/* Case-insensitive substring. */
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
    /* A bare pattern also matches a path segment. */
    if (strcmp(rel, name) != 0 && fnmatch(pat, rel, flags) == 0)
        return true;
    return false;
}

static bool file_pattern_ok(const ssess *s, const char *name)
{
    if (!s->file_pattern || !*s->file_pattern)
        return true;
    /* filePattern accepts alternatives separated by | or , */
    char *copy = xstrdup(s->file_pattern);
    bool ok = false;
    for (char *save = NULL, *tok = strtok_r(copy, "|,", &save); tok && !ok;
         tok = strtok_r(NULL, "|,", &save)) {
        while (*tok == ' ')
            tok++;
        if (fnmatch(tok, name, FNM_CASEFOLD) == 0)
            ok = true;
    }
    free(copy);
    return ok;
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
    if (!data) {
        free(err);
        return;
    }
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
        if (regcomp(&re, s->pattern, flags) == 0)
            use_re = true;
    }

    /* Split into lines. */
    char **lines = NULL;
    size_t nlines = 0, lcap = 0;
    char *p = data;
    while (p && *p) {
        char *nl = strchr(p, '\n');
        if (nl)
            *nl = '\0';
        if (nlines == lcap) {
            lcap = lcap ? lcap * 2 : 128;
            lines = xrealloc(lines, lcap * sizeof *lines);
        }
        size_t l = strlen(p);
        if (l && p[l - 1] == '\r')
            p[l - 1] = '\0';
        lines[nlines++] = p;
        if (!nl)
            break;
        p = nl + 1;
    }

    long long last_shown = -1;
    for (size_t i = 0; i < nlines; i++) {
        bool hit;
        if (use_re)
            hit = regexec(&re, lines[i], 0, NULL, 0) == 0;
        else if (s->ignore_case)
            hit = contains_ci(lines[i], s->pattern);
        else
            hit = strstr(lines[i], s->pattern) != NULL;

        if (hit) {
            if (s->context > 0) {
                long long from = (long long)i - s->context;
                if (from < 0)
                    from = 0;
                for (long long k = from; k < (long long)i; k++) {
                    if (k <= last_shown)
                        continue;
                    char *row = xasprintf("%s:%lld · %s", rel, k + 1, lines[k]);
                    row_push(s, row, true);
                    last_shown = k;
                }
            }
            char *row = xasprintf("%s:%zu - %s", rel, i + 1, lines[i]);
            row_push(s, row, false);
            last_shown = (long long)i;

            long long after_end = (long long)i + s->context;
            for (long long k = (long long)i + 1; k <= after_end && k < (long long)nlines; k++) {
                char *crow = xasprintf("%s:%lld · %s", rel, k + 1, lines[k]);
                row_push(s, crow, true);
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

    DIR *d = opendir(dir);
    if (!d)
        return;

    char **names = NULL;
    size_t n = 0, cap = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        if (!s->hidden && file_hidden(de->d_name))
            continue;
        if (strcmp(de->d_name, ".git") == 0 && !s->hidden)
            continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 32;
            names = xrealloc(names, cap * sizeof *names);
        }
        names[n++] = xstrdup(de->d_name);
    }
    closedir(d);

    for (size_t i = 0; i < n && !sees_stop(s); i++) {
        char *full = *rel ? xasprintf("%s/%s", dir, names[i]) : xasprintf("%s/%s", dir, names[i]);
        char *child_rel = *rel ? xasprintf("%s/%s", rel, names[i]) : xstrdup(names[i]);
        struct stat st;
        bool isdir = false;
        if (lstat(full, &st) == 0) {
            isdir = S_ISDIR(st.st_mode);
            if (!isdir) {
                if (s->content) {
                    if (file_pattern_ok(s, names[i]))
                        content_search_file(s, full, child_rel);
                } else if (name_matches(s, names[i], child_rel) && file_pattern_ok(s, names[i])) {
                    char *row = xasprintf("%s", child_rel);
                    row_push(s, row, false);
                    if (s->early_termination) {
                        pthread_mutex_lock(&s->lock);
                        if (str_ieq(names[i], s->pattern)) {
                            s->exact_hit = true;
                            pthread_cond_broadcast(&s->cond);
                            pthread_mutex_unlock(&s->lock);
                            break;
                        }
                        pthread_mutex_unlock(&s->lock);
                    }
                }
            }
        }
        if (isdir)
            walk(s, full, child_rel);
        free(child_rel);
        free(full);
    }

    for (size_t i = 0; i < n; i++)
        free(names[i]);
    free(names);
}

static void *search_main(void *arg)
{
    ssess *s = arg;
    struct stat st;
    if (stat(s->root, &st) == 0 && S_ISREG(st.st_mode)) {
        if (s->content)
            content_search_file(s, s->root, s->root);
        else if (name_matches(s, base_name(s->root), base_name(s->root))) {
            char *row = xstrdup(s->root);
            row_push(s, row, false);
        }
    } else {
        walk(s, s->root, "");
    }
    pthread_mutex_lock(&s->lock);
    s->done = true;
    pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->lock);
    return NULL;
}

static ssess *find_session(const char *id)
{
    for (ssess *s = g_sessions; s; s = s->next)
        if (strcmp(s->id, id) == 0)
            return s;
    return NULL;
}

tool_result tool_start_search(json *a)
{
    const char *path = jstr_get(a, "path", NULL);
    const char *pattern = jstr_get(a, "pattern", NULL);
    if (!path || !*path)
        return result_error("Missing path");
    if (!pattern)
        return result_error("Missing pattern");

    char *root = NULL;
    char *err = validate_path(path, &root);
    if (err) {
        tool_result r = result_error("%s", err);
        free(err);
        return r;
    }
    struct stat st;
    if (stat(root, &st) != 0) {
        tool_result r = result_error("Search path does not exist: %s", path);
        free(root);
        return r;
    }

    ssess *s = xmalloc(sizeof *s);
    memset(s, 0, sizeof *s);
    s->root = root;
    s->pattern = xstrdup(pattern);
    const char *type = jstr_get(a, "searchType", "files");
    s->content = strcmp(type, "content") == 0;

    json *lf = jget(a, "literalSearch");
    s->literal = lf && lf->t == J_BOOL ? lf->u.b : false;
    json *ic = jget(a, "ignoreCase");
    s->ignore_case = ic && ic->t == J_BOOL ? ic->u.b : true;
    json *hid = jget(a, "includeHidden");
    s->hidden = hid && hid->t == J_BOOL ? hid->u.b : false;
    json *et = jget(a, "earlyTermination");
    s->early_termination = et && et->t == J_BOOL ? et->u.b : !s->content;

    const char *fp = jstr_get(a, "filePattern", NULL);
    s->file_pattern = fp ? xstrdup(fp) : NULL;
    s->max_results = jint_get(a, "maxResults", 0);
    s->timeout_ms = jint_get(a, "timeout_ms", 0);
    s->context = jint_get(a, "contextLines", 5);
    if (s->context < 0)
        s->context = 0;
    s->started_ms = now_ms();
    pthread_mutex_init(&s->lock, NULL);
    pthread_cond_init(&s->cond, NULL);

    pthread_mutex_lock(&g_sessions_lock);
    snprintf(s->id, sizeof s->id, "search-%ld-%lu", (long)getpid(), ++g_seq);
    s->next = g_sessions;
    g_sessions = s;
    pthread_mutex_unlock(&g_sessions_lock);

    if (pthread_create(&s->thread, NULL, search_main, s) != 0) {
        tool_result r = result_error("Cannot start the search thread");
        return r;
    }
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
    size_t shown = s->n < 100 ? s->n : 100;
    bool done = s->done;
    long long matches = s->matches;
    pthread_mutex_unlock(&s->lock);

    sbuf b;
    sb_init(&b);
    sb_printf(&b, "Started %s search session: %s\n", s->content ? "content" : "file", s->id);
    sb_printf(&b, "Pattern: \"%s\"\nPath: %s\n", pattern, path);
    sb_printf(&b, "Status: %s\nRuntime: %lldms\n", done ? "COMPLETED" : "RUNNING", now_ms() - s->started_ms);
    sb_printf(&b, "Total results: %lld %s\n", matches, matches == 1 ? "match" : "matches");
    if (shown > 0) {
        sb_put(&b, "\nInitial results:\n");
        pthread_mutex_lock(&s->lock);
        for (size_t i = 0; i < shown; i++)
            sb_printf(&b, "%s\n", s->rows[i].row);
        pthread_mutex_unlock(&s->lock);
    }
    if (!done)
        sb_put(&b, "\nSearch in progress. Use get_more_search_results to get more.");
    else
        sb_put(&b, "\nSearch completed.");

    tool_result r = result_text(b.p);
    sb_free(&b);
    return r;
}

tool_result tool_get_more_search_results(json *a)
{
    const char *id = jstr_get(a, "sessionId", NULL);
    if (!id)
        return result_error("Missing sessionId");
    long long offset = jint_get(a, "offset", 0);
    long long length = jint_get(a, "length", 100);
    if (offset < 0)
        offset = 0;
    if (length < 1)
        length = 100;

    pthread_mutex_lock(&g_sessions_lock);
    ssess *s = find_session(id);
    pthread_mutex_unlock(&g_sessions_lock);
    if (!s)
        return result_error("Search session %s not found", id);

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
        while (!s->done && s->n < want)
            if (pthread_cond_timedwait(&s->cond, &s->lock, &until) == ETIMEDOUT)
                break;
    }
    size_t total = s->n;
    size_t from = (size_t)offset < total ? (size_t)offset : total;
    size_t count = (size_t)length;
    if (from + count > total)
        count = total - from;
    bool done = s->done;
    bool stopped = s->stopped, timed_out = s->timed_out, maxed = s->maxed;
    bool exact_hit = s->exact_hit;
    long long matches = s->matches;
    long long timeout = s->timeout_ms;

    sbuf b;
    sb_init(&b);
    if (count == 0) {
        if (done)
            sb_printf(&b, "No results at offset %lld (%zu in total).", offset, total);
        else
            sb_printf(&b, "Still running: %zu rows so far, none at offset %lld yet.", total, offset);
    } else {
        sb_printf(&b, "[Showing %zu rows from %zu (total: %zu rows, %zu remaining)]\n\n",
                  count, from, total, total - (from + count));
        for (size_t i = from; i < from + count; i++)
            sb_printf(&b, "%s\n", s->rows[i].row);
        if (from + count < total)
            sb_printf(&b, "\nMore results available. Use get_more_search_results with offset: %zu",
                      from + count);
        else if (!done)
            sb_put(&b, "\nStill running: more may come.");
    }
    if (done) {
        if (maxed)
            sb_printf(&b, "\nStopped at maxResults (%lld): there may be more.", matches);
        else if (timed_out)
            sb_printf(&b, "\nStopped after %lld ms: results may be incomplete.", timeout);
        else if (stopped)
            sb_put(&b, "\nStopped on request: results may be incomplete.");
        else if (exact_hit)
            sb_put(&b, "\nStopped early: exact filename match.");
        else
            sb_put(&b, "\nSearch completed.");
    }
    pthread_mutex_unlock(&s->lock);

    tool_result r = result_text(b.p);
    sb_free(&b);
    return r;
}

tool_result tool_stop_search(json *a)
{
    const char *id = jstr_get(a, "sessionId", NULL);
    if (!id)
        return result_error("Missing sessionId");
    pthread_mutex_lock(&g_sessions_lock);
    ssess *s = find_session(id);
    pthread_mutex_unlock(&g_sessions_lock);
    if (!s) {
        char *m = xasprintf("Search session %s not found.", id);
        tool_result r = result_text(m);
        free(m);
        return r;
    }

    pthread_mutex_lock(&s->lock);
    bool done = s->done;
    s->stopped = true;
    pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->lock);

    char *msg = done ? xasprintf("Search session %s already completed.", id)
                     : xasprintf("Search session %s terminated successfully.", id);
    tool_result r = result_text(msg);
    free(msg);
    return r;
}
tool_result tool_list_searches(json *a)
{
    (void)a;
    pthread_mutex_lock(&g_sessions_lock);
    if (!g_sessions) {
        pthread_mutex_unlock(&g_sessions_lock);
        return result_text("No active searches.");
    }
    sbuf b;
    sb_init(&b);
    size_t count = 0;
    for (ssess *s = g_sessions; s; s = s->next)
        count++;
    sb_printf(&b, "Searches (%zu):\n\n", count);
    for (ssess *s = g_sessions; s; s = s->next) {
        pthread_mutex_lock(&s->lock);
        sb_printf(&b, "Session: %s\n  Type: %s\n  Pattern: \"%s\"\n  Status: %s\n"
                      "  Runtime: %llds\n  Results: %lld\n\n",
                  s->id, s->content ? "content" : "files", s->pattern,
                  s->done ? "COMPLETED" : "RUNNING",
                  (now_ms() - s->started_ms) / 1000, s->matches);
        pthread_mutex_unlock(&s->lock);
    }
    pthread_mutex_unlock(&g_sessions_lock);
    tool_result r = result_text(b.p);
    sb_free(&b);
    return r;
}
