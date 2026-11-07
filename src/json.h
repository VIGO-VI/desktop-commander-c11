/* SPDX-License-Identifier: MIT
 * Copyright 2026 the Desktop Commander C contributors
 *
 * JSON (RFC 8259) for one request: a document arena of nodes addressed by
 * index, and the strings they point at. No node and no string is allocated on
 * its own, so freeing is one call and the next request reuses everything.
 */
#ifndef DCM_JSON_H
#define DCM_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A node handle: an index into its document. JNULL is the null node, so a
 * missing key and a JSON null read the same, and an index is 4 bytes on every
 * target. */
typedef uint32_t jref;
#define JNULL ((jref)0)

typedef enum { J_NULL, J_BOOL, J_INT, J_REAL, J_STR, J_ARR, J_OBJ } jtype;

typedef struct jdoc jdoc;

jdoc *jdoc_new(void);
void jdoc_free(jdoc *d);
/* Keeps the buffers for the next request; the handles of this one die. */
void jdoc_reset(jdoc *d);

/* Parses text into d. Returns false and a static message in *err on failure. */
bool json_parse(jdoc *d, const char *text, size_t len, jref *out, const char **err);

jref jnull(jdoc *d);
jref jbool(jdoc *d, bool b);
jref jint(jdoc *d, int64_t i);
jref jreal(jdoc *d, double x);
jref jstr(jdoc *d, const char *s);
jref jstrn(jdoc *d, const char *s, size_t n);
jref jarr(jdoc *d);
jref jobj(jdoc *d);
jref jarr_push(jdoc *d, jref arr, jref v);
/* Sets k to v, replacing the value of an existing key. Returns obj. */
jref jobj_set(jdoc *d, jref obj, const char *k, jref v);
bool jobj_del(jdoc *d, jref obj, const char *k);
/* Removes k and returns its value, or JNULL. The node itself stays in d. */
jref jobj_take(jdoc *d, jref obj, const char *k);

jtype jtype_of(const jdoc *d, jref v);
bool jis_obj(const jdoc *d, jref v);
bool jis_arr(const jdoc *d, jref v);
bool jis_str(const jdoc *d, jref v);

/* The string of v, or "" when v is not a string. The pointer is stable until
 * the document is reset or freed. */
const char *jstr_of(const jdoc *d, jref v);
double jnum_of(const jdoc *d, jref v);
int64_t jint_of(const jdoc *d, jref v, int64_t dflt);
bool jbool_of(const jdoc *d, jref v);

jref jget(jdoc *d, jref obj, const char *k);
const char *jstr_get(jdoc *d, jref obj, const char *k, const char *dflt);
int64_t jint_get(jdoc *d, jref obj, const char *k, int64_t dflt);
double jnum_get(jdoc *d, jref obj, const char *k, double dflt);
bool jbool_get(jdoc *d, jref obj, const char *k, bool dflt);

size_t jlen(const jdoc *d, jref v);
jref jidx(const jdoc *d, jref v, size_t i);
const char *jkey(const jdoc *d, jref v, size_t i);

/* Small growable byte buffer. It keeps its capacity between resets, so a
 * server of steady traffic allocates once per thread. */
typedef struct {
    char *p;
    size_t n, cap;
} sbuf;

void sb_init(sbuf *b);
void sb_free(sbuf *b);
void sb_reset(sbuf *b);
void sb_putc(sbuf *b, char c);
void sb_put(sbuf *b, const char *s);
void sb_putn(sbuf *b, const char *s, size_t n);
void sb_printf(sbuf *b, const char *fmt, ...);
void sb_json_str(sbuf *b, const char *s);
char *sb_take(sbuf *b);

/* Appends v to b. Nothing allocates per node. */
void json_dump_into(sbuf *b, const jdoc *d, jref v);

#endif
