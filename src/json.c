/* SPDX-License-Identifier: MIT
 * Copyright 2026 the Desktop Commander C contributors
 *
 * JSON for one request. A document is three linear arrays: nodes, child
 * chunks and bytes. Handles are 4-byte indices, so a document costs the same
 * on a 32-bit target, and handing it back is `jdoc_reset` with no walk.
 *
 * Children of one container are 16-entry chunks linked in order: a container
 * that goes on hold while another is built keeps its place and appends in O(1).
 */
#include "config.h"
#include "json.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JSON_MAX_DEPTH 256
#define CHUNK_SHIFT 4
#define CHUNK_ENTS (1u << CHUNK_SHIFT)

typedef struct {
    jref v;
    uint32_t key; /* string handle, or 0 for an array element */
} jent;

typedef struct {
    jent e[CHUNK_ENTS];
    uint32_t next; /* chunk index + 1, or 0 */
} jchunk;

typedef struct {
    int64_t num;   /* an integer, or the bits of a real */
    uint32_t a;    /* string handle, or the first chunk + 1 */
    uint32_t n;    /* string length, or element count */
    uint32_t last; /* the last chunk + 1, for a container */
    uint8_t t;
} jnode;

struct jdoc {
    jnode *nodes;
    uint32_t nnodes, capnodes;
    jchunk *chunks;
    uint32_t nchunks, capchunks;
    char *pool;
    uint32_t pool_cap, pool_used;
    char **big;
    uint32_t nbig, capbig;
};

#define STR_BIG 0x80000000u

void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);

/* ---------------------------------------------------------------- buffer */

void sb_init(sbuf *b) { b->p = NULL; b->n = b->cap = 0; }

void sb_free(sbuf *b)
{
    free(b->p);
    sb_init(b);
}

void sb_reset(sbuf *b)
{
    b->n = 0;
    if (b->p)
        b->p[0] = '\0';
}

static void sb_reserve(sbuf *b, size_t extra)
{
    if (extra > SIZE_MAX / 2 - b->cap)
        abort();
    if (b->n + extra + 1 <= b->cap)
        return;
    size_t cap = b->cap ? b->cap : 64;
    while (cap < b->n + extra + 1)
        cap *= 2;
    b->p = xrealloc(b->p, cap);
    b->cap = cap;
}

void sb_putn(sbuf *b, const char *s, size_t n)
{
    if (!n)
        return;
    sb_reserve(b, n);
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = '\0';
}

void sb_putc(sbuf *b, char c) { sb_putn(b, &c, 1); }
void sb_put(sbuf *b, const char *s) { sb_putn(b, s, strlen(s)); }

void sb_printf(sbuf *b, const char *fmt, ...)
{
    char tmp[256];
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int need = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (need < 0) {
        va_end(ap2);
        return;
    }
    if ((size_t)need < sizeof tmp) {
        sb_putn(b, tmp, (size_t)need);
    } else {
        sb_reserve(b, (size_t)need);
        vsnprintf(b->p + b->n, (size_t)need + 1, fmt, ap2);
        b->n += (size_t)need;
    }
    va_end(ap2);
}

char *sb_take(sbuf *b)
{
    char *p = b->p;
    if (!p) {
        p = xmalloc(1);
        p[0] = '\0';
    }
    sb_init(b);
    return p;
}

/* Length of the UTF-8 sequence at p, or 0 when the bytes there are not one.
 * Overlong forms, surrogates and code points above U+10FFFF are invalid, so a
 * string that this accepts is what RFC 8259 asks of a JSON text. */
static size_t utf8_seq(const unsigned char *p, size_t n)
{
    unsigned char c = p[0];
    if (c < 0x80)
        return 1;
    if (c < 0xC2)
        return 0;
    if (c < 0xE0) {
        if (n < 2 || (p[1] & 0xC0) != 0x80)
            return 0;
        return 2;
    }
    if (c < 0xF0) {
        if (n < 3 || (p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80)
            return 0;
        if (c == 0xE0 && p[1] < 0xA0)
            return 0;
        if (c == 0xED && p[1] > 0x9F)
            return 0;
        return 3;
    }
    if (c < 0xF5) {
        if (n < 4 || (p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80 ||
            (p[3] & 0xC0) != 0x80)
            return 0;
        if (c == 0xF0 && p[1] < 0x90)
            return 0;
        if (c == 0xF4 && p[1] > 0x8F)
            return 0;
        return 4;
    }
    return 0;
}

/* A string of the answer. Every byte that is not valid UTF-8 becomes U+FFFD,
 * so a file or a process that prints a byte from another encoding still
 * yields a line the client can parse. */
void sb_json_str(sbuf *b, const char *s)
{
    sb_putc(b, '"');
    size_t n = s ? strlen(s) : 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"': sb_put(b, "\\\""); break;
        case '\\': sb_put(b, "\\\\"); break;
        case '\n': sb_put(b, "\\n"); break;
        case '\r': sb_put(b, "\\r"); break;
        case '\t': sb_put(b, "\\t"); break;
        case '\b': sb_put(b, "\\b"); break;
        case '\f': sb_put(b, "\\f"); break;
        default:
            if (c < 0x20) {
                sb_printf(b, "\\u%04x", (unsigned)c);
            } else if (c < 0x80) {
                sb_putc(b, (char)c);
            } else {
                size_t len = utf8_seq((const unsigned char *)s + i, n - i);
                if (len) {
                    sb_putn(b, s + i, len);
                    i += len - 1;
                } else {
                    sb_put(b, "\\ufffd");
                }
            }
        }
    }
    sb_putc(b, '"');
}

/* ---------------------------------------------------------------- strings */

static char *str_at(const jdoc *d, uint32_t ref)
{
    return (ref & STR_BIG) ? d->big[ref & ~STR_BIG] : d->pool + ref;
}

/* Reserves n + 1 bytes and returns them, or NULL when n cannot be a handle. */
static char *str_alloc(jdoc *d, size_t n)
{
    if (n > 0x7FFFFFFEu)
        return NULL;
    if (!d->pool) {
        d->pool_cap = DCM_REQUEST_STRINGS;
        d->pool = xmalloc(d->pool_cap);
        d->pool[0] = '\0';
        d->pool_used = 1; /* byte 0 is never a handle, so 0 can mean "none" */
    }
    if (n + 1 <= (size_t)(d->pool_cap - d->pool_used)) {
        char *p = d->pool + d->pool_used;
        d->pool_used += (uint32_t)n + 1;
        return p;
    }
    if (d->nbig == d->capbig) {
        d->capbig = d->capbig ? d->capbig * 2 : 8;
        d->big = xrealloc(d->big, (size_t)d->capbig * sizeof *d->big);
    }
    d->big[d->nbig] = xmalloc(n + 1);
    return d->big[d->nbig];
}

/* The handle of bytes that str_alloc returned for d. Never 0. */
static uint32_t str_handle(jdoc *d, char *p)
{
    if (p >= d->pool && p < d->pool + d->pool_cap)
        return (uint32_t)(p - d->pool);
    return STR_BIG | (d->nbig++);
}

static uint32_t str_put(jdoc *d, const char *s, size_t n)
{
    char *p = str_alloc(d, n);
    if (!p)
        return 0;
    memcpy(p, s, n);
    p[n] = '\0';
    return str_handle(d, p);
}

/* ---------------------------------------------------------------- nodes */

void jdoc_reset(jdoc *d)
{
    if (!d->nodes) {
        d->capnodes = 256;
        d->nodes = xmalloc((size_t)d->capnodes * sizeof *d->nodes);
    }
    d->nodes[0] = (jnode){0}; /* node 0 is null, so JNULL needs no case */
    d->nnodes = 1;
    d->nchunks = 0;
    d->pool_used = d->pool ? 1 : 0;
    for (uint32_t i = 0; i < d->nbig; i++)
        free(d->big[i]);
    d->nbig = 0;
    if (d->pool)
        d->pool[0] = '\0';
}

jdoc *jdoc_new(void)
{
    jdoc *d = xmalloc(sizeof *d);
    memset(d, 0, sizeof *d);
    jdoc_reset(d);
    return d;
}

void jdoc_free(jdoc *d)
{
    if (!d)
        return;
    jdoc_reset(d);
    free(d->big);
    free(d->pool);
    free(d->chunks);
    free(d->nodes);
    free(d);
}

static jref node_new(jdoc *d, jtype t)
{
    if (d->nnodes == 0)
        jdoc_reset(d);
    if (d->nnodes >= DCM_REQUEST_NODES)
        return JNULL;
    if (d->nnodes == d->capnodes) {
        d->capnodes *= 2;
        d->nodes = xrealloc(d->nodes, (size_t)d->capnodes * sizeof *d->nodes);
    }
    jref r = d->nnodes++;
    d->nodes[r] = (jnode){.t = (uint8_t)t};
    return r;
}

jref jnull(jdoc *d) { (void)d; return JNULL; }
jref jarr(jdoc *d) { return node_new(d, J_ARR); }
jref jobj(jdoc *d) { return node_new(d, J_OBJ); }

jref jbool(jdoc *d, bool b)
{
    jref r = node_new(d, J_BOOL);
    d->nodes[r].n = b != 0;
    return r;
}

jref jint(jdoc *d, int64_t i)
{
    jref r = node_new(d, J_INT);
    d->nodes[r].num = i;
    return r;
}

jref jreal(jdoc *d, double x)
{
    jref r = node_new(d, J_REAL);
    uint64_t bits;
    memcpy(&bits, &x, sizeof bits);
    d->nodes[r].num = (int64_t)bits;
    return r;
}

jref jstrn(jdoc *d, const char *s, size_t n)
{
    if (!s)
        return JNULL;
    uint32_t h = str_put(d, s, n);
    if (!h)
        return JNULL;
    jref r = node_new(d, J_STR);
    if (r == JNULL)
        return r;
    d->nodes[r].a = h;
    d->nodes[r].n = (uint32_t)n;
    return r;
}

jref jstr(jdoc *d, const char *s) { return jstrn(d, s ? s : "", s ? strlen(s) : 0); }

static jent *ent_at(jdoc *d, uint32_t first, uint32_t i)
{
    uint32_t c = first;
    while (i >= CHUNK_ENTS) {
        c = d->chunks[c].next - 1;
        i -= CHUNK_ENTS;
    }
    return &d->chunks[c].e[i];
}

static const jent *ent_cat(const jdoc *d, uint32_t first, uint32_t i)
{
    uint32_t c = first;
    while (i >= CHUNK_ENTS) {
        c = d->chunks[c].next - 1;
        i -= CHUNK_ENTS;
    }
    return &d->chunks[c].e[i];
}

static void chunk_push(jdoc *d, jref cont, jent e)
{
    jnode *node = &d->nodes[cont];
    if (node->n % CHUNK_ENTS == 0) {
        if (d->nchunks == d->capchunks) {
            d->capchunks = d->capchunks ? d->capchunks * 2 : 16;
            d->chunks = xrealloc(d->chunks, (size_t)d->capchunks * sizeof *d->chunks);
        }
        uint32_t c = d->nchunks++;
        memset(&d->chunks[c], 0, sizeof d->chunks[c]);
        if (node->last)
            d->chunks[node->last - 1].next = c + 1;
        else
            node->a = c + 1;
        node->last = c + 1;
    }
    d->chunks[node->last - 1].e[node->n % CHUNK_ENTS] = e;
    node->n++;
}

jref jarr_push(jdoc *d, jref a, jref v)
{
    if (jtype_of(d, a) == J_ARR)
        chunk_push(d, a, (jent){v, 0});
    return a;
}

static bool key_is(const jdoc *d, uint32_t key, const char *k)
{
    return key != 0 && strcmp(str_at(d, key), k) == 0;
}

jref jobj_set(jdoc *d, jref o, const char *k, jref v)
{
    if (jtype_of(d, o) != J_OBJ)
        return o;
    for (uint32_t i = 0; i < d->nodes[o].n; i++) {
        jent *e = ent_at(d, d->nodes[o].a - 1, i);
        if (key_is(d, e->key, k)) {
            e->v = v;
            return o;
        }
    }
    uint32_t h = str_put(d, k, strlen(k));
    chunk_push(d, o, (jent){v, h});
    return o;
}

/* The index of k in o, or the entry count when it is absent. */
static uint32_t obj_index(jdoc *d, jref o, const char *k)
{
    if (jtype_of(d, o) != J_OBJ)
        return 0;
    for (uint32_t i = 0; i < d->nodes[o].n; i++)
        if (key_is(d, ent_cat(d, d->nodes[o].a - 1, i)->key, k))
            return i;
    return d->nodes[o].n;
}

/* Drops entry skip: the later entries move one place back in one walk. */
static jref ent_remove(jdoc *d, jref o, uint32_t skip)
{
    uint32_t first = d->nodes[o].a - 1, n = d->nodes[o].n;
    jref removed = ent_at(d, first, skip)->v;
    for (uint32_t i = skip + 1; i < n; i++)
        *ent_at(d, first, i - 1) = *ent_at(d, first, i);
    d->nodes[o].n = n - 1;
    return removed;
}

jref jobj_take(jdoc *d, jref o, const char *k)
{
    uint32_t i = obj_index(d, o, k);
    return i < jlen(d, o) ? ent_remove(d, o, i) : JNULL;
}

bool jobj_del(jdoc *d, jref o, const char *k)
{
    uint32_t i = obj_index(d, o, k);
    if (i >= jlen(d, o))
        return false;
    ent_remove(d, o, i);
    return true;
}

/* ---------------------------------------------------------------- readers */

jtype jtype_of(const jdoc *d, jref v)
{
    return (!d || v == JNULL || v >= d->nnodes) ? J_NULL : (jtype)d->nodes[v].t;
}

bool jis_obj(const jdoc *d, jref v) { return jtype_of(d, v) == J_OBJ; }
bool jis_arr(const jdoc *d, jref v) { return jtype_of(d, v) == J_ARR; }
bool jis_str(const jdoc *d, jref v) { return jtype_of(d, v) == J_STR; }

const char *jstr_of(const jdoc *d, jref v)
{
    return jtype_of(d, v) == J_STR ? str_at(d, d->nodes[v].a) : "";
}

double jnum_of(const jdoc *d, jref v)
{
    if (jtype_of(d, v) == J_REAL) {
        uint64_t bits = (uint64_t)d->nodes[v].num;
        double x;
        memcpy(&x, &bits, sizeof x);
        return x;
    }
    return jtype_of(d, v) == J_INT ? (double)d->nodes[v].num : 0;
}

bool jbool_of(const jdoc *d, jref v) { return jtype_of(d, v) == J_BOOL && d->nodes[v].n; }

int64_t jint_of(const jdoc *d, jref v, int64_t dflt)
{
    jtype t = jtype_of(d, v);
    return t == J_INT ? d->nodes[v].num : t == J_REAL ? (int64_t)jnum_of(d, v) : dflt;
}

jref jget(jdoc *d, jref o, const char *k)
{
    uint32_t i = obj_index(d, o, k);
    return i < jlen(d, o) ? ent_cat(d, d->nodes[o].a - 1, i)->v : JNULL;
}

const char *jstr_get(jdoc *d, jref o, const char *k, const char *dflt)
{
    jref v = jget(d, o, k);
    return jis_str(d, v) ? jstr_of(d, v) : dflt;
}

int64_t jint_get(jdoc *d, jref o, const char *k, int64_t dflt)
{
    return jint_of(d, jget(d, o, k), dflt);
}

double jnum_get(jdoc *d, jref o, const char *k, double dflt)
{
    jref v = jget(d, o, k);
    jtype t = jtype_of(d, v);
    return (t == J_REAL || t == J_INT) ? jnum_of(d, v) : dflt;
}

bool jbool_get(jdoc *d, jref o, const char *k, bool dflt)
{
    jref v = jget(d, o, k);
    return jtype_of(d, v) == J_BOOL ? d->nodes[v].n != 0 : dflt;
}

size_t jlen(const jdoc *d, jref v)
{
    jtype t = jtype_of(d, v);
    return (t == J_ARR || t == J_OBJ) ? d->nodes[v].n : 0;
}

jref jidx(const jdoc *d, jref v, size_t i)
{
    if (jtype_of(d, v) != J_ARR || i >= d->nodes[v].n)
        return JNULL;
    return ent_cat(d, d->nodes[v].a - 1, (uint32_t)i)->v;
}

const char *jkey(const jdoc *d, jref v, size_t i)
{
    if (jtype_of(d, v) != J_OBJ || i >= d->nodes[v].n)
        return "";
    uint32_t key = ent_cat(d, d->nodes[v].a - 1, (uint32_t)i)->key;
    return key ? str_at(d, key) : "";
}

/* ---------------------------------------------------------------- parse */

typedef struct {
    jdoc *d;
    const char *p, *end, *err;
    int depth;
} parser;

static bool perr(parser *ps, const char *msg)
{
    if (!ps->err)
        ps->err = msg;
    return false;
}

static void skip_ws(parser *ps)
{
    while (ps->p < ps->end && (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' ||
                               *ps->p == '\r'))
        ps->p++;
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
}

/* Reads four hex digits at ps->p and advances. */
static bool hex4(parser *ps, unsigned *v)
{
    unsigned x = 0;
    for (int i = 0; i < 4; i++) {
        if (ps->p >= ps->end)
            return perr(ps, "truncated \\u escape");
        int h = hexval((unsigned char)*ps->p++);
        if (h < 0)
            return perr(ps, "bad \\u escape");
        x = (x << 4) | (unsigned)h;
    }
    *v = x;
    return true;
}

/* Reads one \\uXXXX at ps->p, with ps->p just past the 'u'. */
static bool read_uescape(parser *ps, unsigned *cp)
{
    unsigned v, lo;
    if (!hex4(ps, &v))
        return false;
    *cp = v;
    if (v < 0xD800 || v > 0xDFFF)
        return true;
    if (v <= 0xDBFF && ps->end - ps->p >= 2 && ps->p[0] == '\\' && ps->p[1] == 'u') {
        ps->p += 2;
        if (!hex4(ps, &lo))
            return false;
        *cp = (lo >= 0xDC00 && lo <= 0xDFFF) ? 0x10000 + ((v - 0xD800) << 10) + (lo - 0xDC00)
                                             : 0xFFFD;
        return true;
    }
    *cp = 0xFFFD;
    return true;
}

static size_t utf8_len(unsigned cp)
{
    return cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
}

static void utf8_write(char *p, unsigned cp)
{
    if (cp < 0x80) {
        p[0] = (char)cp;
        return;
    }
    if (cp < 0x800) {
        p[1] = (char)(0x80 | (cp & 0x3F));
        p[0] = (char)(0xC0 | (cp >> 6));
        return;
    }
    if (cp < 0x10000) {
        p[2] = (char)(0x80 | (cp & 0x3F));
        p[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        p[0] = (char)(0xE0 | (cp >> 12));
        return;
    }
    p[3] = (char)(0x80 | (cp & 0x3F));
    p[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    p[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    p[0] = (char)(0xF0 | (cp >> 18));
}

/* Measures the string at ps->p (past its quote) and leaves ps->p on the
 * closing quote. */
static bool measure_string(parser *ps, size_t *out)
{
    size_t n = 0;
    while (ps->p < ps->end) {
        unsigned char c = (unsigned char)*ps->p++;
        if (c == '"') {
            *out = n;
            ps->p--;
            return true;
        }
        if (c < 0x20)
            return perr(ps, "control character in string");
        if (c != '\\') {
            n++;
            continue;
        }
        if (ps->p >= ps->end)
            return perr(ps, "truncated escape");
        char e = *ps->p++;
        if (strchr("\"\\/bfnrt", e)) {
            n++;
        } else if (e == 'u') {
            unsigned cp = 0;
            if (!read_uescape(ps, &cp))
                return false;
            n += utf8_len(cp);
        } else {
            return perr(ps, "unknown escape");
        }
    }
    return perr(ps, "unterminated string");
}

static bool parse_string(parser *ps, uint32_t *handle)
{
    if (ps->p >= ps->end || *ps->p != '"')
        return perr(ps, "expected string");
    const char *start = ++ps->p;
    size_t n = 0;
    if (!measure_string(ps, &n))
        return false;
    char *dst = str_alloc(ps->d, n);
    if (!dst)
        return perr(ps, "string too long");
    char *w = dst;
    for (ps->p = start; *ps->p != '"';) {
        unsigned char c = (unsigned char)*ps->p++;
        if (c != '\\') {
            *w++ = (char)c;
            continue;
        }
        char e = *ps->p++;
        switch (e) {
        case 'b': *w++ = '\b'; break;
        case 'f': *w++ = '\f'; break;
        case 'n': *w++ = '\n'; break;
        case 'r': *w++ = '\r'; break;
        case 't': *w++ = '\t'; break;
        case 'u': {
            unsigned cp = 0;
            if (!read_uescape(ps, &cp))
                return false;
            utf8_write(w, cp);
            w += utf8_len(cp);
            break;
        }
        default: *w++ = e; break; /* " \ / */
        }
    }
    ps->p++;
    *w = '\0';
    *handle = str_handle(ps->d, dst);
    return true;
}

static jref parse_number(parser *ps)
{
    const char *start = ps->p;
    bool real = false;
    if (ps->p < ps->end && *ps->p == '-')
        ps->p++;
    while (ps->p < ps->end && *ps->p >= '0' && *ps->p <= '9')
        ps->p++;
    if (ps->p < ps->end && *ps->p == '.') {
        real = true;
        ps->p++;
        while (ps->p < ps->end && *ps->p >= '0' && *ps->p <= '9')
            ps->p++;
    }
    if (ps->p < ps->end && (*ps->p == 'e' || *ps->p == 'E')) {
        real = true;
        ps->p++;
        if (ps->p < ps->end && (*ps->p == '+' || *ps->p == '-'))
            ps->p++;
        while (ps->p < ps->end && *ps->p >= '0' && *ps->p <= '9')
            ps->p++;
    }
    if (ps->p == start) {
        perr(ps, "expected value");
        return JNULL;
    }
    char tmp[64];
    size_t n = (size_t)(ps->p - start);
    if (n >= sizeof tmp)
        n = sizeof tmp - 1;
    memcpy(tmp, start, n);
    tmp[n] = '\0';
    if (!real) {
        char *endp = NULL;
        long long i = strtoll(tmp, &endp, 10);
        if (endp && *endp == '\0')
            return jint(ps->d, i);
    }
    return jreal(ps->d, strtod(tmp, NULL));
}

static jref parse_into(parser *ps);

/* An array or an object: the only difference is the key of each element. */
static jref parse_container(parser *ps, bool obj)
{
    jref c = obj ? jobj(ps->d) : jarr(ps->d);
    if (c == JNULL)
        return JNULL;
    ps->p++;
    skip_ws(ps);
    if (ps->p < ps->end && *ps->p == (obj ? '}' : ']')) {
        ps->p++;
        return c;
    }
    for (;;) {
        skip_ws(ps);
        uint32_t k = 0;
        if (obj) {
            if (!parse_string(ps, &k))
                return JNULL;
            skip_ws(ps);
            if (ps->p >= ps->end || *ps->p != ':') {
                perr(ps, "expected :");
                return JNULL;
            }
            ps->p++;
        }
        jref v = parse_into(ps);
        if (ps->err)
            return JNULL;
        chunk_push(ps->d, c, (jent){v, k});
        skip_ws(ps);
        if (ps->p >= ps->end) {
            perr(ps, obj ? "unterminated object" : "unterminated array");
            return JNULL;
        }
        if (*ps->p == ',') {
            ps->p++;
            continue;
        }
        if (*ps->p == (obj ? '}' : ']')) {
            ps->p++;
            return c;
        }
        perr(ps, obj ? "expected , or }" : "expected , or ]");
        return JNULL;
    }
}

static jref parse_into(parser *ps)
{
    skip_ws(ps);
    if (ps->p >= ps->end) {
        perr(ps, "unexpected end of input");
        return JNULL;
    }
    if (++ps->depth > JSON_MAX_DEPTH) {
        perr(ps, "nesting too deep");
        return JNULL;
    }
    jref v = JNULL;
    char c = *ps->p;
    if (c == '{' || c == '[') {
        v = parse_container(ps, c == '{');
    } else if (c == '"') {
        uint32_t h = 0;
        if (parse_string(ps, &h)) {
            v = node_new(ps->d, J_STR);
            if (v == JNULL)
                perr(ps, "request too large");
            else
                ps->d->nodes[v].a = h;
        }
    } else if (c == 't' && ps->end - ps->p >= 4 && memcmp(ps->p, "true", 4) == 0) {
        ps->p += 4;
        v = jbool(ps->d, true);
    } else if (c == 'f' && ps->end - ps->p >= 5 && memcmp(ps->p, "false", 5) == 0) {
        ps->p += 5;
        v = jbool(ps->d, false);
    } else if (c == 'n' && ps->end - ps->p >= 4 && memcmp(ps->p, "null", 4) == 0) {
        ps->p += 4;
    } else if (c == '-' || (c >= '0' && c <= '9')) {
        v = parse_number(ps);
    } else {
        perr(ps, "unexpected character");
    }
    ps->depth--;
    return ps->err ? JNULL : v;
}

bool json_parse(jdoc *d, const char *text, size_t len, jref *out, const char **err)
{
    if (err)
        *err = NULL;
    jdoc_reset(d);
    parser ps = {.d = d, .p = text, .end = text + len};
    if (len >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB &&
        (unsigned char)text[2] == 0xBF)
        ps.p += 3;
    jref v = parse_into(&ps);
    if (!ps.err) {
        skip_ws(&ps);
        if (ps.p != ps.end)
            perr(&ps, "trailing characters after value");
    }
    if (ps.err) {
        if (err)
            *err = ps.err;
        return false;
    }
    *out = v;
    return true;
}

/* ---------------------------------------------------------------- dump */

static void dump_into(sbuf *b, const jdoc *d, jref v, int depth)
{
    jtype t = jtype_of(d, v);
    switch (t) {
    case J_NULL: sb_put(b, "null"); return;
    case J_BOOL: sb_put(b, jbool_of(d, v) ? "true" : "false"); return;
    case J_INT: sb_printf(b, "%" PRId64, d->nodes[v].num); return;
    case J_REAL: {
        char tmp[40];
        snprintf(tmp, sizeof tmp, "%.17g", jnum_of(d, v));
        sb_put(b, tmp);
        return;
    }
    case J_STR: sb_json_str(b, jstr_of(d, v)); return;
    default: break;
    }
    if (depth > JSON_MAX_DEPTH) {
        sb_put(b, "null");
        return;
    }
    bool obj = t == J_OBJ;
    uint32_t n = d->nodes[v].n, first = d->nodes[v].a - 1;
    sb_putc(b, obj ? '{' : '[');
    for (uint32_t i = 0; i < n; i++) {
        if (i)
            sb_putc(b, ',');
        if (obj) {
            sb_json_str(b, jkey(d, v, i));
            sb_putc(b, ':');
        }
        dump_into(b, d, ent_cat(d, first, i)->v, depth + 1);
    }
    sb_putc(b, obj ? '}' : ']');
}

void json_dump_into(sbuf *b, const jdoc *d, jref v)
{
    dump_into(b, d, v, 0);
}
