/* SPDX-License-Identifier: MIT
 * Copyright 2026 the Desktop Commander C contributors
 *
 * The configuration of this build. Every value comes from src/config.h, which
 * the Makefile copies from src/config.def.h. Nothing is read from disk and
 * nothing can be set at run time, so a tool never allocates a config.
 */
#include "config.h"
#include "util.h"

#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>

/* Each entry of the list in config.h ends with a comma, and the NULL closes
 * it. An empty DCM_BLOCKED_COMMANDS is { NULL }: nothing is blocked. */
static const char *const k_blocked[] = {DCM_BLOCKED_COMMANDS NULL};
static const char *const k_allowed[] = {DCM_ALLOWED_DIRECTORIES NULL};

static config g_cfg;
static char *g_roots[sizeof k_allowed / sizeof k_allowed[0]];
static char g_shell[PATH_MAX];
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static void cfg_init(void)
{
    g_cfg.blocked = k_blocked;
    g_cfg.nblocked = sizeof k_blocked / sizeof k_blocked[0] - 1;

    if (*DCM_DEFAULT_SHELL) {
        snprintf(g_shell, sizeof g_shell, "%s", DCM_DEFAULT_SHELL);
    } else {
        const char *sh = getenv("SHELL");
        snprintf(g_shell, sizeof g_shell, "%s", (sh && *sh) ? sh : "/bin/bash");
    }
    g_cfg.shell = g_shell;
    g_cfg.read_line_limit = DCM_READ_LINE_LIMIT;
    g_cfg.write_line_limit = DCM_WRITE_LINE_LIMIT;
    g_cfg.output_lines = DCM_OUTPUT_LINES;
    g_cfg.output_bytes = DCM_OUTPUT_BYTES;

    size_t n = 0;
    for (size_t i = 0; i + 1 < sizeof k_allowed / sizeof k_allowed[0]; i++) {
        char *real = realpath(k_allowed[i], NULL);
        if (real) {
            g_roots[n++] = real;
        } else {
            fprintf(stderr, "dcm: allowedDirectories: %s does not resolve\n", k_allowed[i]);
        }
    }
    g_cfg.allowed = (const char *const *)g_roots;
    g_cfg.nallowed = n;
}

const config *cfg(void)
{
    pthread_once(&g_once, cfg_init);
    return &g_cfg;
}

jref config_json(jdoc *d)
{
    const config *c = cfg();
    jref o = jobj(d);

    jref blocked = jarr(d);
    for (size_t i = 0; i < c->nblocked; i++)
        jarr_push(d, blocked, jstr(d, c->blocked[i]));
    jobj_set(d, o, "blockedCommands", blocked);

    jref allowed = jarr(d);
    for (size_t i = 0; i < c->nallowed; i++)
        jarr_push(d, allowed, jstr(d, c->allowed[i]));
    jobj_set(d, o, "allowedDirectories", allowed);

    jobj_set(d, o, "defaultShell", jstr(d, c->shell));
    jobj_set(d, o, "fileReadLineLimit", jint(d, c->read_line_limit));
    jobj_set(d, o, "fileWriteLineLimit", jint(d, c->write_line_limit));
    jobj_set(d, o, "maxOutputLines", jint(d, c->output_lines));
    jobj_set(d, o, "maxOutputBytes", jint(d, c->output_bytes));
    jobj_set(d, o, "version", jstr(d, DCM_VERSION));

    jref sys = jobj(d);
    struct utsname u;
    if (uname(&u) == 0) {
        jobj_set(d, sys, "platform", jstr(d, u.sysname));
        jobj_set(d, sys, "release", jstr(d, u.release));
        jobj_set(d, sys, "arch", jstr(d, u.machine));
    }
    jobj_set(d, sys, "shell", jstr(d, c->shell));
    char host[256] = {0};
    if (gethostname(host, sizeof host - 1) == 0)
        jobj_set(d, sys, "hostname", jstr(d, host));
    jobj_set(d, o, "systemInfo", sys);

    jobj_set(d, o, "telemetry", jstr(d, "none: this build has no telemetry"));
    jobj_set(d, o, "configSource", jstr(d, "build time: src/config.h"));
    return o;
}
