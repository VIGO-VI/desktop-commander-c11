/* SPDX-License-Identifier: MIT
 * Copyright 2026 the Desktop Commander C contributors
 *
 * Configuration tools and the local call history. The configuration is the one
 * compiled in (src/config.h). The history is a fixed ring in this process:
 * nothing is written to disk and nothing leaves the machine.
 */
#include "config.h"
#include "tools.h"
#include "util.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SUMMARY_MAX 96

typedef struct {
    const char *tool; /* a string literal of the tool table */
    int64_t at_ms;
    bool ok;
    char summary[SUMMARY_MAX];
} hentry;

static pthread_mutex_t g_hist_lock = PTHREAD_MUTEX_INITIALIZER;
static hentry g_hist[DCM_HISTORY_ENTRIES];
static size_t g_hist_head; /* the slot the next call takes */
static size_t g_hist_count;
static size_t g_hist_total;
static int64_t g_started_ms;

/* The newest entry first: entry i is (head - 1 - i) modulo the ring. */
static hentry *hist_at(size_t i)
{
    size_t back = g_hist_head + DCM_HISTORY_ENTRIES - 1 - i;
    return &g_hist[back % DCM_HISTORY_ENTRIES];
}

/* Writes "key=value, ..." for at most four argument keys, without allocating. */
static void summarise_args(jdoc *d, jref args, char *out, size_t cap)
{
    if (!jis_obj(d, args)) {
        snprintf(out, cap, "{}");
        return;
    }
    size_t at = 0;
    size_t shown = 0;
    for (size_t i = 0; i < jlen(d, args) && shown < 4 && at + 1 < cap; i++) {
        const char *key = jkey(d, args, i);
        if (strcmp(key, "content") == 0)
            continue;
        jref v = jidx(d, args, i);
        int n = snprintf(out + at, cap - at, "%s%s=", shown ? ", " : "", key);
        if (n < 0 || (size_t)n >= cap - at)
            break;
        at += (size_t)n;
        shown++;
        if (jis_str(d, v)) {
            const char *s = jstr_of(d, v);
            size_t len = strlen(s);
            if (len > 80) {
                n = snprintf(out + at, cap - at, "%.77s...", s);
            } else {
                n = snprintf(out + at, cap - at, "%s", s);
            }
        } else if (jtype_of(d, v) == J_INT) {
            n = snprintf(out + at, cap - at, "%lld", (long long)jint_of(d, v, 0));
        } else if (jtype_of(d, v) == J_BOOL) {
            n = snprintf(out + at, cap - at, "%s", jbool_of(d, v) ? "true" : "false");
        } else {
            n = snprintf(out + at, cap - at, "...");
        }
        if (n < 0 || (size_t)n >= cap - at)
            break;
        at += (size_t)n;
    }
    out[at] = '\0';
}

void history_record(jdoc *d, const char *tool, jref args, const tool_result *r)
{
    char summary[SUMMARY_MAX];
    summarise_args(d, args, summary, sizeof summary);
    int64_t now = now_ms();

    pthread_mutex_lock(&g_hist_lock);
    if (g_started_ms == 0)
        g_started_ms = now;
    hentry *e = &g_hist[g_hist_head];
    e->tool = tool;
    e->at_ms = now;
    e->ok = !r->is_error;
    memcpy(e->summary, summary, sizeof summary);
    g_hist_head = (g_hist_head + 1) % DCM_HISTORY_ENTRIES;
    if (g_hist_count < DCM_HISTORY_ENTRIES)
        g_hist_count++;
    g_hist_total++;
    pthread_mutex_unlock(&g_hist_lock);
}

void history_shutdown(void)
{
    /* Every entry is inline: there is nothing to free. */
    pthread_mutex_lock(&g_hist_lock);
    g_hist_count = 0;
    g_hist_head = 0;
    pthread_mutex_unlock(&g_hist_lock);
}

tool_result tool_get_config(jdoc *d, jref a)
{
    (void)a;
    sbuf b;
    sb_init(&b);
    sb_put(&b, "Current configuration:\n");
    json_dump_into(&b, d, config_json(d), true);
    tool_result r = result_text(d, b.p ? b.p : "");
    sb_free(&b);
    return r;
}

tool_result tool_set_config_value(jdoc *d, jref a)
{
    (void)a;
    return result_error(d,
                        "Cannot set a value at run time: this build takes its configuration "
                        "from src/config.h. Edit config.h (copied from config.def.h by make) "
                        "and rebuild.");
}

tool_result tool_get_usage_stats(jdoc *d, jref a)
{
    (void)a;
    jref per_tool = jobj(d);
    int64_t total, ok = 0, uptime;

    pthread_mutex_lock(&g_hist_lock);
    total = (int64_t)g_hist_total;
    for (size_t i = 0; i < g_hist_count; i++) {
        hentry *e = hist_at(i);
        ok += e->ok;
        jobj_set(d, per_tool, e->tool,
                 jint(d, jint_of(d, jget(d, per_tool, e->tool), 0) + 1));
    }
    uptime = g_started_ms ? (now_ms() - g_started_ms) / 1000 : 0;
    pthread_mutex_unlock(&g_hist_lock);

    sbuf b;
    sb_init(&b);
    sb_printf(&b, "Session uptime: %llds\nCalls: %lld (%lld ok, %lld failed)\n",
              (long long)uptime, (long long)total, (long long)ok, (long long)(total - ok));

    jref root = jobj(d);
    jobj_set(d, root, "uptimeSeconds", jint(d, uptime));
    jobj_set(d, root, "totalCalls", jint(d, total));
    jobj_set(d, root, "successfulCalls", jint(d, ok));
    jobj_set(d, root, "failedCalls", jint(d, total - ok));
    jobj_set(d, root, "tools", per_tool);

    tool_result r = result_text_struct(d, b.p, root);
    sb_free(&b);
    return r;
}

static int64_t parse_iso_ms(const char *s)
{
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    int y, mo, day, h, mi, sec;
    if (sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &mo, &day, &h, &mi, &sec) != 6)
        return -1;
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = day;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_sec = sec;
    return (int64_t)timegm(&tm) * 1000;
}

tool_result tool_get_recent_tool_calls(jdoc *d, jref a)
{
    int64_t max = jint_get(d, a, "maxResults", 50);
    if (max < 1)
        max = 1;
    if (max > 1000)
        max = 1000;
    const char *only = jstr_get(d, a, "toolName", NULL);
    const char *since = jstr_get(d, a, "since", NULL);
    int64_t since_ms = since ? parse_iso_ms(since) : -1;

    jref calls = jarr(d);
    sbuf body;
    sb_init(&body);
    int64_t shown = 0;

    pthread_mutex_lock(&g_hist_lock);
    for (size_t i = 0; i < g_hist_count && shown < max; i++) {
        hentry *e = hist_at(i);
        if (only && strcmp(only, e->tool) != 0)
            continue;
        if (since_ms >= 0 && e->at_ms < since_ms)
            continue;
        char iso[32];
        time_t t = (time_t)(e->at_ms / 1000);
        struct tm tm;
        gmtime_r(&t, &tm);
        strftime(iso, sizeof iso, "%Y-%m-%dT%H:%M:%SZ", &tm);

        jref c = jobj(d);
        jobj_set(d, c, "tool", jstr(d, e->tool));
        jobj_set(d, c, "timestamp", jstr(d, iso));
        jobj_set(d, c, "success", jbool(d, e->ok));
        jobj_set(d, c, "arguments", jstr(d, e->summary));
        jarr_push(d, calls, c);
        sb_printf(&body, "- %s %s %s(%s)\n", iso, e->ok ? "ok" : "error", e->tool, e->summary);
        shown++;
    }
    pthread_mutex_unlock(&g_hist_lock);

    sbuf b;
    sb_init(&b);
    if (shown == 0) {
        sb_put(&b, "No tool calls recorded.");
    } else {
        sb_printf(&b, "Recent tool calls (%lld):\n", (long long)shown);
        sb_putn(&b, body.p, body.n);
    }
    sb_free(&body);

    jref root = jobj(d);
    jobj_set(d, root, "calls", calls);
    tool_result r = result_text_struct(d, b.p, root);
    sb_free(&b);
    return r;
}
