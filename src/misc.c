/* SPDX-License-Identifier: MIT
 * Copyright 2026 the Desktop Commander C contributors
 *
 * Configuration tools and the local call history. There is no telemetry: the
 * history stays in this process and in ~/.config/desktop-commander/history.jsonl.
 */
#include "tools.h"
#include "util.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define HISTORY_MAX 200

typedef struct {
    char *tool;
    long long at_ms;
    bool ok;
    char *summary;
} hentry;

static pthread_mutex_t g_hist_lock = PTHREAD_MUTEX_INITIALIZER;
static hentry g_hist[HISTORY_MAX];
static size_t g_hist_count;   /* number of valid entries, newest at g_hist[0] */
static size_t g_hist_total;
static long long g_started_ms;

static char *summarise_args(const json *args)
{
    if (!args || args->t != J_OBJ)
        return xstrdup("{}");
    sbuf b;
    sb_init(&b);
    size_t shown = 0;
    for (size_t i = 0; i < args->u.o.n && shown < 4; i++) {
        const char *key = args->u.o.k[i];
        json *v = args->u.o.v[i];
        if (strcmp(key, "content") == 0)
            continue;
        if (shown++)
            sb_put(&b, ", ");
        sb_printf(&b, "%s=", key);
        if (v->t == J_STR) {
            const char *s = v->u.s;
            size_t n = strlen(s);
            if (n > 80) {
                sb_putn(&b, s, 77);
                sb_put(&b, "...");
            } else {
                sb_put(&b, s);
            }
        } else if (v->t == J_INT) {
            sb_printf(&b, "%lld", v->u.i);
        } else if (v->t == J_BOOL) {
            sb_put(&b, v->u.b ? "true" : "false");
        } else {
            sb_put(&b, "...");
        }
    }
    return sb_take(&b);
}

void history_record(const char *tool, const json *args, const tool_result *r)
{
    if (g_started_ms == 0)
        g_started_ms = now_ms();

    hentry e;
    e.tool = xstrdup(tool);
    e.at_ms = now_ms();
    e.ok = !r->is_error;
    e.summary = summarise_args(args);

    pthread_mutex_lock(&g_hist_lock);
    if (g_hist_count == HISTORY_MAX) {
        free(g_hist[HISTORY_MAX - 1].tool);
        free(g_hist[HISTORY_MAX - 1].summary);
        memmove(&g_hist[1], &g_hist[0], (HISTORY_MAX - 1) * sizeof *g_hist);
        g_hist_count--;
    }
    memmove(&g_hist[1], &g_hist[0], g_hist_count * sizeof *g_hist);
    g_hist[0] = e;
    g_hist_count++;
    g_hist_total++;
    pthread_mutex_unlock(&g_hist_lock);
}

void history_shutdown(void)
{
    pthread_mutex_lock(&g_hist_lock);
    for (size_t i = 0; i < g_hist_count; i++) {
        free(g_hist[i].tool);
        free(g_hist[i].summary);
    }
    g_hist_count = 0;
    pthread_mutex_unlock(&g_hist_lock);
}

tool_result tool_get_config(json *a)
{
    (void)a;
    config *cfg = config_load();
    json *root = config_to_json(cfg);
    config_free(cfg);
    char *body = json_dump(root, true);
    json_free(root);
    char *text = xasprintf("Current configuration:\n%s", body);
    free(body);
    tool_result r = result_text(text);
    free(text);
    return r;
}

tool_result tool_set_config_value(json *a)
{
    const char *key = jstr_get(a, "key", NULL);
    if (!key || !*key)
        return result_error("Missing key");
    json *value = jget(a, "value");
    if (!value)
        return result_error("Missing value");

    config *cfg = config_load();
    char *err = NULL;
    if (!config_set(cfg, key, value, &err)) {
        tool_result r = result_error("%s", err);
        free(err);
        config_free(cfg);
        return r;
    }
    if (!config_save(cfg, &err)) {
        tool_result r = result_error("%s", err);
        free(err);
        config_free(cfg);
        return r;
    }
    config_free(cfg);

    char *text = json_dump(value, false);
    char *msg = xasprintf("Set %s to %s", key, text);
    tool_result r = result_text(msg);
    free(msg);
    free(text);
    return r;
}

tool_result tool_get_usage_stats(json *a)
{
    (void)a;
    pthread_mutex_lock(&g_hist_lock);
    long long total = (long long)g_hist_total;
    long long ok = 0;
    json *per_tool = jobj();
    for (size_t i = 0; i < g_hist_count; i++) {
        if (g_hist[i].ok)
            ok++;
        json *cur = jget(per_tool, g_hist[i].tool);
        jobj_set(per_tool, g_hist[i].tool,
                 jint(cur && cur->t == J_INT ? cur->u.i + 1 : 1));
    }
    pthread_mutex_unlock(&g_hist_lock);

    sbuf b;
    sb_init(&b);
    long long uptime = g_started_ms ? (now_ms() - g_started_ms) / 1000 : 0;
    sb_printf(&b, "Session uptime: %llds\nCalls: %lld (%lld ok, %lld failed)\n",
              uptime, total, ok, total - ok);

    json *root = jobj();
    jobj_set(root, "uptimeSeconds", jint(uptime));
    jobj_set(root, "totalCalls", jint(total));
    jobj_set(root, "successfulCalls", jint(ok));
    jobj_set(root, "failedCalls", jint(total - ok));
    jobj_set(root, "tools", per_tool);

    tool_result r = result_text_struct(b.p, root);
    sb_free(&b);
    return r;
}

static long long parse_iso_ms(const char *s)
{
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    int y, mo, d, h, mi, sec;
    if (sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &sec) != 6)
        return -1;
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_sec = sec;
    return (long long)timegm(&tm) * 1000;
}

tool_result tool_get_recent_tool_calls(json *a)
{
    long long max = jint_get(a, "maxResults", 50);
    if (max < 1)
        max = 1;
    if (max > 1000)
        max = 1000;
    const char *only = jstr_get(a, "toolName", NULL);
    const char *since = jstr_get(a, "since", NULL);
    long long since_ms = since ? parse_iso_ms(since) : -1;

    json *calls = jarr();
    pthread_mutex_lock(&g_hist_lock);
    long long shown = 0;
    for (size_t i = 0; i < g_hist_count && shown < max; i++) {
        if (only && strcmp(only, g_hist[i].tool) != 0)
            continue;
        if (since_ms >= 0 && g_hist[i].at_ms < since_ms)
            continue;
        json *c = jobj();
        jobj_set(c, "tool", jstr(g_hist[i].tool));
        char iso[64];
        time_t t = (time_t)(g_hist[i].at_ms / 1000);
        struct tm tm;
        gmtime_r(&t, &tm);
        strftime(iso, sizeof iso, "%Y-%m-%dT%H:%M:%SZ", &tm);
        jobj_set(c, "timestamp", jstr(iso));
        jobj_set(c, "success", jbool(g_hist[i].ok));
        jobj_set(c, "arguments", jstr(g_hist[i].summary));
        jarr_push(calls, c);
        shown++;
    }
    pthread_mutex_unlock(&g_hist_lock);

    sbuf b;
    sb_init(&b);
    if (shown == 0) {
        sb_put(&b, "No tool calls recorded.");
    } else {
        sb_printf(&b, "Recent tool calls (%lld):\n", shown);
        for (size_t i = 0; i < calls->u.a.n; i++) {
            json *c = calls->u.a.v[i];
            sb_printf(&b, "- %s %s %s(%s)\n",
                      jstr_get(c, "timestamp", ""),
                      jbool_get(c, "success", false) ? "ok" : "error",
                      jstr_get(c, "tool", ""),
                      jstr_get(c, "arguments", ""));
        }
    }
    json *root = jobj();
    jobj_set(root, "calls", calls);
    tool_result r = result_text_struct(b.p, root);
    sb_free(&b);
    return r;
}
