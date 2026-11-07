/* SPDX-License-Identifier: MIT
 * Copyright 2026 the Desktop Commander C contributors
 */
#ifndef DCM_UTIL_H
#define DCM_UTIL_H

#include "json.h"

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#define DCM_VERSION "1.0.0"

typedef struct {
    char **blocked;
    size_t nblocked;
    char *default_shell;
    char **allowed;
    size_t nallowed;
    long long read_line_limit;  /* fileReadLineLimit */
    long long write_line_limit; /* fileWriteLineLimit */
} config;

/* config.c */
config *config_load(void);
void config_free(config *c);
bool config_save(const config *c, char **err);
json *config_to_json(const config *c);
/* Sets one key. Returns false with a message for an unknown key or bad type. */
bool config_set(config *c, const char *key, const json *value, char **err);

/* util.c */
void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xasprintf(const char *fmt, ...);
const char *dcm_config_dir(void);
const char *dcm_home_dir(void);

char *path_expand(const char *path);
char *path_absolute(const char *path);
/* Returns the real path in *out and NULL on success; an error message otherwise. */
char *validate_path(const char *path, char **out);

char *read_file_all(const char *path, size_t *len, char **err);
char *write_file_atomic(const char *path, const char *data, size_t len, bool append,
                        mode_t mode, char **err);

const char *mime_type_for(const char *path);
bool file_is_image(const char *path);
bool data_is_binary(const char *data, size_t len);

char *str_replace_all(const char *hay, const char *from, const char *to,
                      long long expected, long long *count_out);

size_t count_lines(const char *s);
size_t count_lines_upto(const char *s, size_t upto);
/* Number of bytes in the first n lines, ending just after the nth newline. */
size_t bytes_for_lines(const char *s, size_t n);
const char *base_name(const char *path);
char *dir_name(const char *path);

bool str_ieq(const char *a, const char *b);
bool str_iends(const char *s, const char *suffix);
bool str_starts(const char *s, const char *prefix);

/* Command blocklist: 1 when the command line holds a blocked command. */
bool command_is_blocked(const config *c, const char *cmd);
char **command_split(const char *cmd, size_t *n);

long long now_ms(void);

#endif
