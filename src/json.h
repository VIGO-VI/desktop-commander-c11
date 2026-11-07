/* SPDX-License-Identifier: MIT
 * Copyright 2026 the Desktop Commander C contributors
 *
 * Minimal JSON (RFC 8259) reader and writer. No dependencies.
 */
#ifndef DCM_JSON_H
#define DCM_JSON_H

#include <stdbool.h>
#include <stddef.h>

typedef enum { J_NULL, J_BOOL, J_INT, J_REAL, J_STR, J_ARR, J_OBJ } jtype;

typedef struct json {
    jtype t;
    union {
        bool b;
        long long i;
        double d;
        char *s;
        struct {
            struct json **v;
            size_t n, cap;
        } a;
        struct {
            char **k;
            struct json **v;
            size_t n, cap;
        } o;
    } u;
} json;

json *json_parse(const char *text, size_t len, char **err);
void json_free(json *v);

json *jnull(void);
json *jbool(bool b);
json *jint(long long i);
json *jreal(double d);
json *jstr(const char *s);
json *jarr(void);
json *jobj(void);

void jarr_push(json *a, json *v);           /* takes ownership of v */
void jobj_set(json *o, const char *k, json *v); /* takes ownership of v; replaces */
bool jobj_del(json *o, const char *k);
/* Removes k and returns its value without freeing it (NULL when absent). */
json *jobj_take(json *o, const char *k);

json *jget(const json *o, const char *k);   /* borrowed */
const char *jstr_get(const json *o, const char *k, const char *dflt);
long long jint_get(const json *o, const char *k, long long dflt);
double jnum_get(const json *o, const char *k, double dflt);
bool jbool_get(const json *o, const char *k, bool dflt);
size_t jlen(const json *v);

/* Serialisation helpers */
char *json_dump(const json *v, bool pretty);
void json_write_str(char **buf, size_t *len, size_t *cap, const char *s);

/* Small growable byte buffer used across the server. */
typedef struct {
    char *p;
    size_t n, cap;
} sbuf;

void sb_init(sbuf *b);
void sb_free(sbuf *b);
void sb_putc(sbuf *b, char c);
void sb_put(sbuf *b, const char *s);
void sb_putn(sbuf *b, const char *s, size_t n);
void sb_printf(sbuf *b, const char *fmt, ...);
void sb_json_str(sbuf *b, const char *s); /* append a quoted JSON string */
char *sb_take(sbuf *b);

#endif
