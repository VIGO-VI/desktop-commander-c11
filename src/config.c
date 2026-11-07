/* SPDX-License-Identifier: MIT
 * Copyright 2026 the Desktop Commander C contributors
 */
#include "util.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

/* The list of the original server, unchanged: what it blocks, this blocks. */
static const char *const DEFAULT_BLOCKED[] = {
    "mkfs", "format", "mount", "umount", "fdisk", "dd", "parted", "diskpart",
    "sudo", "su", "passwd", "adduser", "useradd", "usermod", "groupadd",
    "chsh", "visudo", "shutdown", "reboot", "halt", "poweroff", "init",
    "iptables", "firewall", "netsh", "sfc", "bcdedit", "reg", "net", "sc",
    "runas", "cipher", "takeown",
};

static void free_list(char **list, size_t n)
{
    for (size_t i = 0; i < n; i++)
        free(list[i]);
    free(list);
}

config *config_load(void)
{
    config *c = xmalloc(sizeof *c);
    memset(c, 0, sizeof *c);

    c->nblocked = sizeof DEFAULT_BLOCKED / sizeof DEFAULT_BLOCKED[0];
    c->blocked = xmalloc(c->nblocked * sizeof *c->blocked);
    for (size_t i = 0; i < c->nblocked; i++)
        c->blocked[i] = xstrdup(DEFAULT_BLOCKED[i]);

    const char *shell = getenv("SHELL");
    c->default_shell = xstrdup(shell && *shell ? shell : "/bin/bash");
    c->read_line_limit = 1000;
    c->write_line_limit = 50;

    char *path = xasprintf("%s/config.json", dcm_config_dir());
    size_t len = 0;
    char *err = NULL;
    char *text = read_file_all(path, &len, &err);
    if (!text) {
        free(err);
        free(path);
        return c;
    }

    char *perr = NULL;
    json *root = json_parse(text, len, &perr);
    free(text);
    free(perr);
    free(path);
    if (!root)
        return c;

    json *v;
    if ((v = jget(root, "blockedCommands")) && v->t == J_ARR) {
        free_list(c->blocked, c->nblocked);
        c->nblocked = jlen(v);
        c->blocked = xmalloc((c->nblocked ? c->nblocked : 1) * sizeof *c->blocked);
        for (size_t i = 0; i < c->nblocked; i++)
            c->blocked[i] = xstrdup(v->u.a.v[i]->t == J_STR ? v->u.a.v[i]->u.s : "");
    }
    if ((v = jget(root, "allowedDirectories")) && v->t == J_ARR) {
        free_list(c->allowed, c->nallowed);
        c->nallowed = jlen(v);
        c->allowed = xmalloc((c->nallowed ? c->nallowed : 1) * sizeof *c->allowed);
        for (size_t i = 0; i < c->nallowed; i++)
            c->allowed[i] = path_absolute(v->u.a.v[i]->t == J_STR ? v->u.a.v[i]->u.s : "");
    }
    if ((v = jget(root, "defaultShell")) && v->t == J_STR && *v->u.s) {
        free(c->default_shell);
        c->default_shell = xstrdup(v->u.s);
    }
    if ((v = jget(root, "fileReadLineLimit")) && v->t == J_INT && v->u.i > 0)
        c->read_line_limit = v->u.i;
    if ((v = jget(root, "fileWriteLineLimit")) && v->t == J_INT && v->u.i > 0)
        c->write_line_limit = v->u.i;

    json_free(root);
    return c;
}

void config_free(config *c)
{
    if (!c)
        return;
    free_list(c->blocked, c->nblocked);
    free_list(c->allowed, c->nallowed);
    free(c->default_shell);
    free(c);
}

json *config_to_json(const config *c)
{
    json *o = jobj();

    json *blocked = jarr();
    for (size_t i = 0; i < c->nblocked; i++)
        jarr_push(blocked, jstr(c->blocked[i]));
    jobj_set(o, "blockedCommands", blocked);

    jobj_set(o, "defaultShell", jstr(c->default_shell));

    json *allowed = jarr();
    for (size_t i = 0; i < c->nallowed; i++)
        jarr_push(allowed, jstr(c->allowed[i]));
    jobj_set(o, "allowedDirectories", allowed);

    jobj_set(o, "fileReadLineLimit", jint(c->read_line_limit));
    jobj_set(o, "fileWriteLineLimit", jint(c->write_line_limit));
    jobj_set(o, "version", jstr(DCM_VERSION));

    struct utsname u;
    json *sys = jobj();
    if (uname(&u) == 0) {
        jobj_set(sys, "platform", jstr(u.sysname));
        jobj_set(sys, "release", jstr(u.release));
        jobj_set(sys, "arch", jstr(u.machine));
    }
    jobj_set(sys, "shell", jstr(c->default_shell));
    char host[256] = {0};
    if (gethostname(host, sizeof host - 1) == 0)
        jobj_set(sys, "hostname", jstr(host));
    jobj_set(o, "systemInfo", sys);
    jobj_set(o, "telemetry", jstr("none: this build has no telemetry"));
    return o;
}

bool config_save(const config *c, char **err)
{
    *err = NULL;
    if (mkdir(dcm_config_dir(), 0700) != 0 && errno != EEXIST) {
        *err = xasprintf("Cannot create %s: %s", dcm_config_dir(), strerror(errno));
        return false;
    }
    char *path = xasprintf("%s/config.json", dcm_config_dir());
    json *root = config_to_json(c);
    char *text = json_dump(root, true);
    json_free(root);
    char *out = write_file_atomic(path, text, strlen(text), false, 0600, err);
    free(text);
    free(path);
    if (!out)
        return false;
    free(out);
    return true;
}

bool config_set(config *c, const char *key, const json *value, char **err)
{
    *err = NULL;
    if (strcmp(key, "blockedCommands") == 0) {
        if (!value || value->t != J_ARR) {
            *err = xstrdup("blockedCommands must be an array of strings");
            return false;
        }
        free_list(c->blocked, c->nblocked);
        c->nblocked = jlen(value);
        c->blocked = xmalloc((c->nblocked ? c->nblocked : 1) * sizeof *c->blocked);
        for (size_t i = 0; i < c->nblocked; i++)
            c->blocked[i] = xstrdup(value->u.a.v[i]->t == J_STR ? value->u.a.v[i]->u.s : "");
        return true;
    }
    if (strcmp(key, "allowedDirectories") == 0) {
        if (!value || value->t != J_ARR) {
            *err = xstrdup("allowedDirectories must be an array of strings");
            return false;
        }
        free_list(c->allowed, c->nallowed);
        c->nallowed = jlen(value);
        c->allowed = xmalloc((c->nallowed ? c->nallowed : 1) * sizeof *c->allowed);
        for (size_t i = 0; i < c->nallowed; i++)
            c->allowed[i] = path_absolute(value->u.a.v[i]->t == J_STR ? value->u.a.v[i]->u.s : "");
        return true;
    }
    if (strcmp(key, "defaultShell") == 0) {
        if (!value || value->t != J_STR || !*value->u.s) {
            *err = xstrdup("defaultShell must be a non-empty string");
            return false;
        }
        free(c->default_shell);
        c->default_shell = xstrdup(value->u.s);
        return true;
    }
    if (strcmp(key, "fileReadLineLimit") == 0 || strcmp(key, "fileWriteLineLimit") == 0) {
        if (!value || value->t != J_INT || value->u.i <= 0) {
            *err = xasprintf("%s must be a positive whole number", key);
            return false;
        }
        if (key[4] == 'R')
            c->read_line_limit = value->u.i;
        else
            c->write_line_limit = value->u.i;
        return true;
    }
    if (strcmp(key, "telemetryEnabled") == 0) {
        *err = xstrdup("telemetry is not implemented in this build and cannot be enabled");
        return false;
    }
    *err = xasprintf("Unknown config key: %s", key);
    return false;
}
