/* SPDX-License-Identifier: MIT
 * Copyright 2026 the Desktop Commander C contributors
 */
#include "../src/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
            failures++;                                                          \
        }                                                                        \
    } while (0)

static jref parse(jdoc *d, const char *text)
{
    const char *err = NULL;
    jref v = JNULL;
    if (!json_parse(d, text, strlen(text), &v, &err))
        fprintf(stderr, "parse failed: %s: %s\n", text, err ? err : "?");
    return v;
}

static void test_parse_scalars(void)
{
    jdoc *d = jdoc_new();
    CHECK(jint_of(d, parse(d, "42"), 0) == 42);
    CHECK(jnum_of(d, parse(d, "-7.5e2")) == -750.0);
    CHECK(jbool_of(d, parse(d, "true")));
    CHECK(jtype_of(d, parse(d, "null")) == J_NULL);
    CHECK(jtype_of(d, parse(d, "9223372036854775807")) == J_INT);
    CHECK(jint_of(d, parse(d, "-9223372036854775807"), 1) == -9223372036854775807LL);
    jdoc_free(d);
}

static void test_parse_string(void)
{
    jdoc *d = jdoc_new();
    CHECK(strcmp(jstr_of(d, parse(d, "\"a\\\"b\\\\c\\n\\t\\u00e9\\u20ac\"")),
                 "a\"b\\c\n\t\xC3\xA9\xE2\x82\xAC") == 0);
    CHECK(strcmp(jstr_of(d, parse(d, "\"\\ud83d\\ude00\"")), "\xF0\x9F\x98\x80") == 0);
    /* A lone surrogate becomes the replacement character. */
    CHECK(strcmp(jstr_of(d, parse(d, "\"\\ud83d\"")), "\xEF\xBF\xBD") == 0);
    jdoc_free(d);
}

static void test_parse_tree(void)
{
    jdoc *d = jdoc_new();
    jref root = parse(d, "{\"a\":[1,2,{\"b\":\"x\"}],\"c\":true,\"d\":null}");
    CHECK(jtype_of(d, root) == J_OBJ);
    jref a = jget(d, root, "a");
    CHECK(jtype_of(d, a) == J_ARR && jlen(d, a) == 3);
    CHECK(jint_of(d, jidx(d, a, 1), 0) == 2);
    CHECK(strcmp(jstr_get(d, jidx(d, a, 2), "b", ""), "x") == 0);
    CHECK(jbool_get(d, root, "c", false));
    CHECK(jtype_of(d, jget(d, root, "d")) == J_NULL);
    CHECK(jget(d, root, "missing") == JNULL);
    jdoc_free(d);
}

static void test_errors(void)
{
    jdoc *d = jdoc_new();
    static const char *const bad[] = {
        "{\"a\":}", "[1,2", "\"unterminated", "{} junk", "tru", "\"a\\q\"", "",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        jref v = JNULL;
        const char *err = NULL;
        CHECK(!json_parse(d, bad[i], strlen(bad[i]), &v, &err));
        CHECK(err != NULL);
    }
    /* Deep nesting is refused rather than overrunning the parser. */
    char deep[1024];
    memset(deep, '[', sizeof deep);
    memset(deep + 500, ']', sizeof deep - 500);
    jref v = JNULL;
    const char *err = NULL;
    CHECK(!json_parse(d, deep, sizeof deep, &v, &err));
    jdoc_free(d);
}

static void test_dump_roundtrip(void)
{
    jdoc *d = jdoc_new();
    jref o = jobj(d);
    jobj_set(d, o, "name", jstr(d, "a \"quoted\"\nstring"));
    jobj_set(d, o, "n", jint(d, 3));
    jref arr = jarr(d);
    jarr_push(d, arr, jbool(d, true));
    jarr_push(d, arr, JNULL);
    jobj_set(d, o, "list", arr);

    sbuf b;
    sb_init(&b);
    json_dump_into(&b, d, o, false);
    CHECK(strcmp(b.p, "{\"name\":\"a \\\"quoted\\\"\\nstring\",\"n\":3,\"list\":[true,null]}")
          == 0);

    jdoc *e = jdoc_new();
    jref back = parse(e, b.p);
    CHECK(back != JNULL);
    CHECK(strcmp(jstr_get(e, back, "name", ""), "a \"quoted\"\nstring") == 0);
    CHECK(jint_get(e, back, "n", 0) == 3);
    CHECK(jlen(e, jget(e, back, "list")) == 2);
    sb_free(&b);
    jdoc_free(e);
    jdoc_free(d);
}

/* A container keeps its children while another one is built inside the loop
 * that fills it. This is the shape every tool result has. */
static void test_interleaved_build(void)
{
    jdoc *d = jdoc_new();
    jref rows = jarr(d);
    for (int i = 0; i < 40; i++) {
        jref row = jobj(d);
        jobj_set(d, row, "i", jint(d, i));
        jobj_set(d, row, "s", jstr(d, "x"));
        jarr_push(d, rows, row);
    }
    CHECK(jlen(d, rows) == 40);
    for (int i = 0; i < 40; i++)
        CHECK(jint_get(d, jidx(d, rows, (size_t)i), "i", -1) == i);
    /* More than one chunk of children, and removal keeps the rest in order. */
    CHECK(jobj_del(d, jidx(d, rows, 3), "i"));
    CHECK(jobj_del(d, jidx(d, rows, 3), "s"));
    CHECK(jlen(d, jidx(d, rows, 3)) == 0);
    CHECK(jint_get(d, jidx(d, rows, 4), "i", -1) == 4);
    jdoc_free(d);
}

static void test_take(void)
{
    jdoc *d = jdoc_new();
    jref o = jobj(d);
    jobj_set(d, o, "x", jstr(d, "keep"));
    jobj_set(d, o, "y", jint(d, 1));
    CHECK(strcmp(jstr_of(d, jobj_take(d, o, "x")), "keep") == 0);
    CHECK(jget(d, o, "x") == JNULL);
    CHECK(jint_get(d, o, "y", 0) == 1);
    jdoc_free(d);
}

/* A long string does not fit the pool of the document: it gets its own block,
 * and its pointer stays valid while more nodes are added. */
static void test_big_string(void)
{
    jdoc *d = jdoc_new();
    size_t n = 300 * 1024;
    char *big = malloc(n + 1);
    memset(big, 'a', n);
    big[n] = '\0';
    jref s = jstr(d, big);
    for (int i = 0; i < 1000; i++)
        jarr_push(d, jarr(d), jstr(d, "filler"));
    const char *back = jstr_of(d, s);
    CHECK(strlen(back) == n);
    CHECK(back[0] == 'a' && back[n - 1] == 'a');
    free(big);
    jdoc_free(d);
}

/* Reset keeps the buffers: a second request of the same size allocates
 * nothing, and handles of the first do not survive. */
static void test_reset(void)
{
    jdoc *d = jdoc_new();
    jref first = parse(d, "{\"a\":1}");
    jdoc_reset(d);
    jref second = parse(d, "{\"b\":2}");
    CHECK(jint_get(d, second, "b", 0) == 2);
    CHECK(jget(d, second, "a") == JNULL);
    CHECK(first == second); /* the same index, from a cleared document */
    jdoc_free(d);
}

static void test_sbuf(void)
{
    sbuf b;
    sb_init(&b);
    sb_printf(&b, "%s-%d", "n", 7);
    sb_json_str(&b, "q\"");
    CHECK(strcmp(b.p, "n-7\"q\\\"\"") == 0);
    /* A format wider than the internal buffer still lands whole. */
    sb_reset(&b);
    char wide[600];
    memset(wide, 'w', sizeof wide - 1);
    wide[sizeof wide - 1] = '\0';
    sb_printf(&b, "%s", wide);
    CHECK(b.n == sizeof wide - 1 && strcmp(b.p, wide) == 0);
    sb_free(&b);
}

int main(void)
{
    test_parse_scalars();
    test_parse_string();
    test_parse_tree();
    test_errors();
    test_dump_roundtrip();
    test_interleaved_build();
    test_take();
    test_big_string();
    test_reset();
    test_sbuf();
    if (failures) {
        fprintf(stderr, "%d json test(s) failed\n", failures);
        return 1;
    }
    puts("json tests: ok");
    return 0;
}
