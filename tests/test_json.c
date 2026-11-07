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

static void test_parse_scalars(void)
{
    char *err = NULL;
    json *v = json_parse("42", 2, &err);
    CHECK(v && v->t == J_INT && v->u.i == 42);
    json_free(v);

    v = json_parse("-7.5e2", 6, &err);
    CHECK(v && v->t == J_REAL && v->u.d == -750.0);
    json_free(v);

    v = json_parse("true", 4, &err);
    CHECK(v && v->t == J_BOOL && v->u.b);
    json_free(v);

    v = json_parse("null", 4, &err);
    CHECK(v && v->t == J_NULL);
    json_free(v);
}

static void test_parse_string(void)
{
    char *err = NULL;
    const char *text = "\"a\\\"b\\\\c\\n\\t\\u00e9\\u20ac\"";
    json *v = json_parse(text, strlen(text), &err);
    CHECK(v && v->t == J_STR);
    if (v)
        CHECK(strcmp(v->u.s, "a\"b\\c\n\t\xC3\xA9\xE2\x82\xAC") == 0);
    json_free(v);
}

static void test_surrogate_pair(void)
{
    char *err = NULL;
    const char *text = "\"\\ud83d\\ude00\"";
    json *v = json_parse(text, strlen(text), &err);
    CHECK(v && v->t == J_STR && strcmp(v->u.s, "\xF0\x9F\x98\x80") == 0);
    json_free(v);
}

static void test_parse_tree(void)
{
    const char *text = "{\"a\":[1,2,{\"b\":\"x\"}],\"c\":true,\"d\":null}";
    char *err = NULL;
    json *root = json_parse(text, strlen(text), &err);
    CHECK(root && root->t == J_OBJ);
    json *a = jget(root, "a");
    CHECK(a && a->t == J_ARR && jlen(a) == 3);
    CHECK(jint_get(a->u.a.v[2], "b", 0) == 0);
    CHECK(strcmp(jstr_get(a->u.a.v[2], "b", ""), "x") == 0);
    CHECK(jbool_get(root, "c", false));
    CHECK(jget(root, "d") && jget(root, "d")->t == J_NULL);
    json_free(root);
}

static void test_errors(void)
{
    char *err = NULL;
    CHECK(json_parse("{\"a\":}", 6, &err) == NULL);
    CHECK(err != NULL);
    free(err);
    err = NULL;
    CHECK(json_parse("[1,2", 4, &err) == NULL);
    free(err);
    err = NULL;
    CHECK(json_parse("\"unterminated", 13, &err) == NULL);
    free(err);
    err = NULL;
    CHECK(json_parse("{} junk", 7, &err) == NULL);
    free(err);
}

static void test_dump_roundtrip(void)
{
    json *o = jobj();
    jobj_set(o, "name", jstr("a \"quoted\"\nstring"));
    jobj_set(o, "n", jint(3));
    json *arr = jarr();
    jarr_push(arr, jbool(true));
    jarr_push(arr, jnull());
    jobj_set(o, "list", arr);

    char *text = json_dump(o, false);
    char *err = NULL;
    json *back = json_parse(text, strlen(text), &err);
    CHECK(back != NULL);
    CHECK(strcmp(jstr_get(back, "name", ""), "a \"quoted\"\nstring") == 0);
    CHECK(jint_get(back, "n", 0) == 3);
    CHECK(jlen(jget(back, "list")) == 2);

    free(text);
    json_free(o);
    json_free(back);
}

static void test_take(void)
{
    json *o = jobj();
    jobj_set(o, "x", jstr("keep"));
    json *taken = jobj_take(o, "x");
    CHECK(taken && strcmp(taken->u.s, "keep") == 0);
    CHECK(jget(o, "x") == NULL);
    json_free(taken);
    json_free(o);
}

static void test_sbuf(void)
{
    sbuf b;
    sb_init(&b);
    sb_printf(&b, "%s-%d", "n", 7);
    sb_json_str(&b, "q\"");
    CHECK(strcmp(b.p, "n-7\"q\\\"\"") == 0);
    sb_free(&b);
}

int main(void)
{
    test_parse_scalars();
    test_parse_string();
    test_surrogate_pair();
    test_parse_tree();
    test_errors();
    test_dump_roundtrip();
    test_take();
    test_sbuf();
    if (failures) {
        fprintf(stderr, "%d json test(s) failed\n", failures);
        return 1;
    }
    puts("json tests: ok");
    return 0;
}
