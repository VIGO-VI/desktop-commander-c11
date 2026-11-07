/* SPDX-License-Identifier: MIT
 * Copyright 2026 the Desktop Commander C contributors
 */
#include "json.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JSON_MAX_DEPTH 256

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) {
        fputs("dcm: out of memory\n", stderr);
        exit(2);
    }
    return p;
}

static void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) {
        fputs("dcm: out of memory\n", stderr);
        exit(2);
    }
    return q;
}

static char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = xmalloc(n);
    memcpy(p, s, n);
    return p;
}

/* ---------------------------------------------------------------- buffer */

void sb_init(sbuf *b)
{
    b->p = NULL;
    b->n = b->cap = 0;
}

void sb_free(sbuf *b)
{
    free(b->p);
    sb_init(b);
}

static void sb_reserve(sbuf *b, size_t extra)
{
    if (b->n + extra + 1 <= b->cap)
        return;
    size_t cap = b->cap ? b->cap : 64;
    while (cap < b->n + extra + 1)
        cap *= 2;
    b->p = xrealloc(b->p, cap);
    b->cap = cap;
}

void sb_putc(sbuf *b, char c)
{
    sb_reserve(b, 1);
    b->p[b->n++] = c;
    b->p[b->n] = '\0';
}

void sb_putn(sbuf *b, const char *s, size_t n)
{
    sb_reserve(b, n);
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = '\0';
}

void sb_put(sbuf *b, const char *s)
{
    sb_putn(b, s, strlen(s));
}

void sb_printf(sbuf *b, const char *fmt, ...)
{
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int need = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (need < 0) {
        va_end(ap2);
        return;
    }
    sb_reserve(b, (size_t)need);
    vsnprintf(b->p + b->n, (size_t)need + 1, fmt, ap2);
    b->n += (size_t)need;
    va_end(ap2);
}

char *sb_take(sbuf *b)
{
    if (!b->p) {
        char *p = xmalloc(1);
        p[0] = '\0';
        return p;
    }
    char *p = b->p;
    sb_init(b);
    return p;
}

static void utf8_put(sbuf *b, unsigned cp)
{
    if (cp < 0x80) {
        sb_putc(b, (char)cp);
    } else if (cp < 0x800) {
        sb_putc(b, (char)(0xC0 | (cp >> 6)));
        sb_putc(b, (char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        sb_putc(b, (char)(0xE0 | (cp >> 12)));
        sb_putc(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
        sb_putc(b, (char)(0x80 | (cp & 0x3F)));
    } else {
        sb_putc(b, (char)(0xF0 | (cp >> 18)));
        sb_putc(b, (char)(0x80 | ((cp >> 12) & 0x3F)));
        sb_putc(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
        sb_putc(b, (char)(0x80 | (cp & 0x3F)));
    }
}

void sb_json_str(sbuf *b, const char *s)
{
    sb_putc(b, '"');
    if (s) {
        for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
            switch (*p) {
            case '"': sb_put(b, "\\\""); break;
            case '\\': sb_put(b, "\\\\"); break;
            case '\n': sb_put(b, "\\n"); break;
            case '\r': sb_put(b, "\\r"); break;
            case '\t': sb_put(b, "\\t"); break;
            case '\b': sb_put(b, "\\b"); break;
            case '\f': sb_put(b, "\\f"); break;
            default:
                if (*p < 0x20)
                    sb_printf(b, "\\u%04x", *p);
                else
                    sb_putc(b, (char)*p);
            }
        }
    }
    sb_putc(b, '"');
}

/* ---------------------------------------------------------------- node */

static json *jnew(jtype t)
{
    json *v = xmalloc(sizeof *v);
    memset(v, 0, sizeof *v);
    v->t = t;
    return v;
}

json *jnull(void) { return jnew(J_NULL); }
json *jbool(bool b) { json *v = jnew(J_BOOL); v->u.b = b; return v; }
json *jint(long long i) { json *v = jnew(J_INT); v->u.i = i; return v; }
json *jreal(double d) { json *v = jnew(J_REAL); v->u.d = d; return v; }
json *jarr(void) { return jnew(J_ARR); }
json *jobj(void) { return jnew(J_OBJ); }

json *jstr(const char *s)
{
    json *v = jnew(J_STR);
    v->u.s = xstrdup(s ? s : "");
    return v;
}

void json_free(json *v)
{
    if (!v)
        return;
    switch (v->t) {
    case J_STR:
        free(v->u.s);
        break;
    case J_ARR:
        for (size_t i = 0; i < v->u.a.n; i++)
            json_free(v->u.a.v[i]);
        free(v->u.a.v);
        break;
    case J_OBJ:
        for (size_t i = 0; i < v->u.o.n; i++) {
            free(v->u.o.k[i]);
            json_free(v->u.o.v[i]);
        }
        free(v->u.o.k);
        free(v->u.o.v);
        break;
    default:
        break;
    }
    free(v);
}

void jarr_push(json *a, json *v)
{
    if (a->t != J_ARR) {
        json_free(v);
        return;
    }
    if (a->u.a.n == a->u.a.cap) {
        a->u.a.cap = a->u.a.cap ? a->u.a.cap * 2 : 4;
        a->u.a.v = xrealloc(a->u.a.v, a->u.a.cap * sizeof *a->u.a.v);
    }
    a->u.a.v[a->u.a.n++] = v;
}

bool jobj_del(json *o, const char *k)
{
    if (o->t != J_OBJ)
        return false;
    for (size_t i = 0; i < o->u.o.n; i++) {
        if (strcmp(o->u.o.k[i], k) == 0) {
            free(o->u.o.k[i]);
            json_free(o->u.o.v[i]);
            memmove(&o->u.o.k[i], &o->u.o.k[i + 1], (o->u.o.n - i - 1) * sizeof *o->u.o.k);
            memmove(&o->u.o.v[i], &o->u.o.v[i + 1], (o->u.o.n - i - 1) * sizeof *o->u.o.v);
            o->u.o.n--;
            return true;
        }
    }
    return false;
}

json *jobj_take(json *o, const char *k)
{
    if (o->t != J_OBJ)
        return NULL;
    for (size_t i = 0; i < o->u.o.n; i++) {
        if (strcmp(o->u.o.k[i], k) == 0) {
            json *v = o->u.o.v[i];
            free(o->u.o.k[i]);
            memmove(&o->u.o.k[i], &o->u.o.k[i + 1], (o->u.o.n - i - 1) * sizeof *o->u.o.k);
            memmove(&o->u.o.v[i], &o->u.o.v[i + 1], (o->u.o.n - i - 1) * sizeof *o->u.o.v);
            o->u.o.n--;
            return v;
        }
    }
    return NULL;
}

void jobj_set(json *o, const char *k, json *v)
{
    if (o->t != J_OBJ) {
        json_free(v);
        return;
    }
    for (size_t i = 0; i < o->u.o.n; i++) {
        if (strcmp(o->u.o.k[i], k) == 0) {
            json_free(o->u.o.v[i]);
            o->u.o.v[i] = v;
            return;
        }
    }
    if (o->u.o.n == o->u.o.cap) {
        o->u.o.cap = o->u.o.cap ? o->u.o.cap * 2 : 4;
        o->u.o.k = xrealloc(o->u.o.k, o->u.o.cap * sizeof *o->u.o.k);
        o->u.o.v = xrealloc(o->u.o.v, o->u.o.cap * sizeof *o->u.o.v);
    }
    o->u.o.k[o->u.o.n] = xstrdup(k);
    o->u.o.v[o->u.o.n] = v;
    o->u.o.n++;
}

json *jget(const json *o, const char *k)
{
    if (!o || o->t != J_OBJ)
        return NULL;
    for (size_t i = 0; i < o->u.o.n; i++)
        if (strcmp(o->u.o.k[i], k) == 0)
            return o->u.o.v[i];
    return NULL;
}

const char *jstr_get(const json *o, const char *k, const char *dflt)
{
    json *v = jget(o, k);
    return (v && v->t == J_STR) ? v->u.s : dflt;
}

long long jint_get(const json *o, const char *k, long long dflt)
{
    json *v = jget(o, k);
    if (!v)
        return dflt;
    if (v->t == J_INT)
        return v->u.i;
    if (v->t == J_REAL)
        return (long long)v->u.d;
    return dflt;
}

double jnum_get(const json *o, const char *k, double dflt)
{
    json *v = jget(o, k);
    if (!v)
        return dflt;
    if (v->t == J_REAL)
        return v->u.d;
    if (v->t == J_INT)
        return (double)v->u.i;
    return dflt;
}

bool jbool_get(const json *o, const char *k, bool dflt)
{
    json *v = jget(o, k);
    return (v && v->t == J_BOOL) ? v->u.b : dflt;
}

size_t jlen(const json *v)
{
    if (!v)
        return 0;
    if (v->t == J_ARR)
        return v->u.a.n;
    if (v->t == J_OBJ)
        return v->u.o.n;
    return 0;
}

/* ---------------------------------------------------------------- parse */

typedef struct {
    const char *p;
    const char *end;
    char *err;
    int depth;
} parser;

static bool perr(parser *ps, const char *msg)
{
    if (!ps->err) {
        sbuf b;
        sb_init(&b);
        sb_printf(&b, "%s", msg);
        ps->err = sb_take(&b);
    }
    return false;
}

static void skip_ws(parser *ps)
{
    while (ps->p < ps->end &&
           (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r'))
        ps->p++;
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static bool parse_hex4(parser *ps, unsigned *out)
{
    if (ps->end - ps->p < 4)
        return perr(ps, "truncated \\u escape");
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        int h = hexval((unsigned char)ps->p[i]);
        if (h < 0)
            return perr(ps, "bad \\u escape");
        v = (v << 4) | (unsigned)h;
    }
    ps->p += 4;
    *out = v;
    return true;
}

static bool parse_string(parser *ps, char **out)
{
    if (ps->p >= ps->end || *ps->p != '"')
        return perr(ps, "expected string");
    ps->p++;
    sbuf b;
    sb_init(&b);
    while (ps->p < ps->end) {
        unsigned char c = (unsigned char)*ps->p++;
        if (c == '"') {
            *out = sb_take(&b);
            return true;
        }
        if (c < 0x20) {
            sb_free(&b);
            return perr(ps, "control character in string");
        }
        if (c != '\\') {
            sb_putc(&b, (char)c);
            continue;
        }
        if (ps->p >= ps->end) {
            sb_free(&b);
            return perr(ps, "truncated escape");
        }
        char e = *ps->p++;
        switch (e) {
        case '"': sb_putc(&b, '"'); break;
        case '\\': sb_putc(&b, '\\'); break;
        case '/': sb_putc(&b, '/'); break;
        case 'b': sb_putc(&b, '\b'); break;
        case 'f': sb_putc(&b, '\f'); break;
        case 'n': sb_putc(&b, '\n'); break;
        case 'r': sb_putc(&b, '\r'); break;
        case 't': sb_putc(&b, '\t'); break;
        case 'u': {
            unsigned cp;
            if (!parse_hex4(ps, &cp)) {
                sb_free(&b);
                return false;
            }
            if (cp >= 0xD800 && cp <= 0xDBFF) {
                unsigned lo;
                if (ps->end - ps->p >= 2 && ps->p[0] == '\\' && ps->p[1] == 'u') {
                    ps->p += 2;
                    if (!parse_hex4(ps, &lo)) {
                        sb_free(&b);
                        return false;
                    }
                    if (lo >= 0xDC00 && lo <= 0xDFFF)
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    else
                        cp = 0xFFFD;
                } else {
                    cp = 0xFFFD;
                }
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                cp = 0xFFFD;
            }
            utf8_put(&b, cp);
            break;
        }
        default:
            sb_free(&b);
            return perr(ps, "unknown escape");
        }
    }
    sb_free(&b);
    return perr(ps, "unterminated string");
}

static json *parse_number(parser *ps)
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
        return NULL;
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
            return jint(i);
    }
    return jreal(strtod(tmp, NULL));
}

static bool parse_into(parser *ps, json **out);

static bool parse_array(parser *ps, json *a)
{
    ps->p++; /* [ */
    skip_ws(ps);
    if (ps->p < ps->end && *ps->p == ']') {
        ps->p++;
        return true;
    }
    for (;;) {
        json *v = NULL;
        if (!parse_into(ps, &v))
            return false;
        jarr_push(a, v);
        skip_ws(ps);
        if (ps->p >= ps->end)
            return perr(ps, "unterminated array");
        if (*ps->p == ',') {
            ps->p++;
            skip_ws(ps);
            continue;
        }
        if (*ps->p == ']') {
            ps->p++;
            return true;
        }
        return perr(ps, "expected , or ]");
    }
}

static bool parse_object(parser *ps, json *o)
{
    ps->p++; /* { */
    skip_ws(ps);
    if (ps->p < ps->end && *ps->p == '}') {
        ps->p++;
        return true;
    }
    for (;;) {
        char *k = NULL;
        skip_ws(ps);
        if (ps->p >= ps->end)
            return perr(ps, "unterminated object");
        if (!parse_string(ps, &k))
            return false;
        skip_ws(ps);
        if (ps->p >= ps->end || *ps->p != ':') {
            free(k);
            return perr(ps, "expected :");
        }
        ps->p++;
        json *v = NULL;
        if (!parse_into(ps, &v)) {
            free(k);
            return false;
        }
        jobj_set(o, k, v);
        free(k);
        skip_ws(ps);
        if (ps->p >= ps->end)
            return perr(ps, "unterminated object");
        if (*ps->p == ',') {
            ps->p++;
            continue;
        }
        if (*ps->p == '}') {
            ps->p++;
            return true;
        }
        return perr(ps, "expected , or }");
    }
}

static bool parse_into(parser *ps, json **out)
{
    skip_ws(ps);
    if (ps->p >= ps->end)
        return perr(ps, "unexpected end of input");
    if (++ps->depth > JSON_MAX_DEPTH)
        return perr(ps, "nesting too deep");

    char c = *ps->p;
    bool ok = true;
    json *v = NULL;

    if (c == '{') {
        v = jobj();
        ok = parse_object(ps, v);
    } else if (c == '[') {
        v = jarr();
        ok = parse_array(ps, v);
    } else if (c == '"') {
        char *s = NULL;
        ok = parse_string(ps, &s);
        if (ok) {
            v = jstr(s);
            free(s);
        }
    } else if (c == 't' && ps->end - ps->p >= 4 && memcmp(ps->p, "true", 4) == 0) {
        ps->p += 4;
        v = jbool(true);
    } else if (c == 'f' && ps->end - ps->p >= 5 && memcmp(ps->p, "false", 5) == 0) {
        ps->p += 5;
        v = jbool(false);
    } else if (c == 'n' && ps->end - ps->p >= 4 && memcmp(ps->p, "null", 4) == 0) {
        ps->p += 4;
        v = jnull();
    } else if (c == '-' || (c >= '0' && c <= '9')) {
        v = parse_number(ps);
        ok = v != NULL;
    } else {
        ok = perr(ps, "unexpected character");
    }

    ps->depth--;
    if (!ok) {
        json_free(v);
        return false;
    }
    *out = v;
    return true;
}

json *json_parse(const char *text, size_t len, char **err)
{
    parser ps = { .p = text, .end = text + len, .err = NULL, .depth = 0 };
    json *v = NULL;
    if (len > 0 && (unsigned char)text[0] == 0xEF && len > 2 &&
        (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) {
        ps.p += 3;
    }
    if (!parse_into(&ps, &v)) {
        if (err)
            *err = ps.err ? ps.err : xstrdup("parse error");
        else
            free(ps.err);
        return NULL;
    }
    skip_ws(&ps);
    if (ps.p != ps.end) {
        json_free(v);
        free(ps.err);
        if (err)
            *err = xstrdup("trailing characters after value");
        return NULL;
    }
    free(ps.err);
    if (err)
        *err = NULL;
    return v;
}

/* ---------------------------------------------------------------- dump */

static void dump_into(sbuf *b, const json *v, bool pretty, int indent)
{
    switch (v->t) {
    case J_NULL:
        sb_put(b, "null");
        break;
    case J_BOOL:
        sb_put(b, v->u.b ? "true" : "false");
        break;
    case J_INT:
        sb_printf(b, "%lld", v->u.i);
        break;
    case J_REAL: {
        char tmp[40];
        snprintf(tmp, sizeof tmp, "%.17g", v->u.d);
        sb_put(b, tmp);
        break;
    }
    case J_STR:
        sb_json_str(b, v->u.s);
        break;
    case J_ARR:
        sb_putc(b, '[');
        for (size_t i = 0; i < v->u.a.n; i++) {
            if (i)
                sb_putc(b, ',');
            if (pretty) {
                sb_putc(b, '\n');
                for (int k = 0; k <= indent; k++)
                    sb_put(b, "  ");
            }
            dump_into(b, v->u.a.v[i], pretty, indent + 1);
        }
        if (pretty && v->u.a.n) {
            sb_putc(b, '\n');
            for (int k = 0; k < indent; k++)
                sb_put(b, "  ");
        }
        sb_putc(b, ']');
        break;
    case J_OBJ:
        sb_putc(b, '{');
        for (size_t i = 0; i < v->u.o.n; i++) {
            if (i)
                sb_putc(b, ',');
            if (pretty) {
                sb_putc(b, '\n');
                for (int k = 0; k <= indent; k++)
                    sb_put(b, "  ");
            }
            sb_json_str(b, v->u.o.k[i]);
            sb_putc(b, ':');
            if (pretty)
                sb_putc(b, ' ');
            dump_into(b, v->u.o.v[i], pretty, indent + 1);
        }
        if (pretty && v->u.o.n) {
            sb_putc(b, '\n');
            for (int k = 0; k < indent; k++)
                sb_put(b, "  ");
        }
        sb_putc(b, '}');
        break;
    }
}

char *json_dump(const json *v, bool pretty)
{
    sbuf b;
    sb_init(&b);
    dump_into(&b, v, pretty, 0);
    return sb_take(&b);
}
