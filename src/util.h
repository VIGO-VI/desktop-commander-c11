/* SPDX-License-Identifier: MIT
 * Copyright 2026 the Desktop Commander C contributors
 */
#ifndef DCM_UTIL_H
#define DCM_UTIL_H

#include "json.h"

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#define DCM_VERSION "2.0.0"

/* The build-time configuration of src/config.h, resolved once. */
typedef struct {
    const char *const *blocked;
    size_t nblocked;
    const char *const *allowed; /* real paths; empty allows everything */
    size_t nallowed;
    const char *shell;
    int64_t read_line_limit;  /* fileReadLineLimit */
    int64_t write_line_limit; /* fileWriteLineLimit */
} config;

const config *cfg(void);
jref config_json(jdoc *d);

void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xasprintf(const char *fmt, ...);
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

/* Command blocklist: true when the first word of a command in the line is
 * blocked. It walks the line once and allocates nothing. */
bool command_is_blocked(const char *cmd);

int64_t now_ms(void);

#endif
