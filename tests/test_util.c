/* SPDX-License-Identifier: MIT
 * Copyright 2026 the Desktop Commander C contributors
 */
#include "../src/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
            failures++;                                                          \
        }                                                                        \
    } while (0)

static void test_lines(void)
{
    CHECK(count_lines("a\nb\nc\n") == 3);
    CHECK(count_lines("a\nb\nc") == 3);
    CHECK(count_lines("") == 0);
    CHECK(bytes_for_lines("aa\nbb\ncc\n", 1) == 3);
    CHECK(bytes_for_lines("aa\nbb\ncc\n", 2) == 6);
    CHECK(count_lines_upto("aa\nbb\ncc\n", 4) == 1);
}

static void test_replace(void)
{
    long long n = 0;
    char *out = str_replace_all("x y x y x", "x", "Z", 3, &n);
    CHECK(n == 3 && out && strcmp(out, "Z y Z y Z") == 0);
    free(out);

    out = str_replace_all("aaa", "aa", "b", 1, &n);
    CHECK(n == 1 && out && strcmp(out, "ba") == 0);
    free(out);

    out = str_replace_all("abc", "zz", "y", 1, &n);
    CHECK(n == 0 && out == NULL);
}

static void test_names(void)
{
    CHECK(strcmp(base_name("/a/b/c.txt"), "c.txt") == 0);
    char *d = dir_name("/a/b/c.txt");
    CHECK(strcmp(d, "/a/b") == 0);
    free(d);
    d = dir_name("/c.txt");
    CHECK(strcmp(d, "/") == 0);
    free(d);
    CHECK(str_ieq("AbC", "abc"));
    CHECK(str_iends("file.PNG", ".png"));
    CHECK(str_starts("hello", "he"));
}

static void test_blocklist(void)
{
    CHECK(command_is_blocked("sudo ls"));
    CHECK(command_is_blocked("echo hi && sudo ls"));
    CHECK(command_is_blocked("ls | mkfs /dev/sda"));
    CHECK(command_is_blocked("echo $(dd if=x)"));
    CHECK(command_is_blocked("'sudo' --version"));
    CHECK(command_is_blocked("SUDO ls"));
    /* The shell starts a new command at a newline, as it does at `;`. */
    CHECK(command_is_blocked("echo hi\nsudo ls"));
    CHECK(command_is_blocked("true\nmount /dev/sda1 /mnt"));
    CHECK(!command_is_blocked("echo sudo"));
    CHECK(!command_is_blocked("ls -la"));
    CHECK(!command_is_blocked("echo \"a; sudo b\""));
    CHECK(!command_is_blocked("echo \\; sudo"));
    CHECK(!command_is_blocked("echo 'a\nsudo b'"));
}

static void test_paths(void)
{
    char *abs = path_absolute("/tmp/../tmp/x");
    /* /tmp is a symlink on some systems, so compare the tail only. */
    CHECK(abs && abs[0] == '/');
    CHECK(str_iends(abs, "/tmp/x"));
    free(abs);

    char *expanded = path_expand("~/x");
    CHECK(strncmp(expanded, dcm_home_dir(), strlen(dcm_home_dir())) == 0);
    free(expanded);

    char *out = NULL;
    char *err = validate_path("/tmp", &out);
    CHECK(err == NULL && out != NULL);
    CHECK(out && str_iends(out, "tmp"));
    free(err);
    free(out);
}

/* The compiled configuration is a constant table: no file, no allocation. */
static void test_config(void)
{
    const config *c = cfg();
    CHECK(c == cfg());
    CHECK(c->nblocked > 0);
    CHECK(c->shell && *c->shell);
    CHECK(c->read_line_limit > 0 && c->write_line_limit > 0);
}

int main(void)
{
    test_lines();
    test_replace();
    test_names();
    test_blocklist();
    test_paths();
    test_config();
    if (failures) {
        fprintf(stderr, "%d util test(s) failed\n", failures);
        return 1;
    }
    puts("util tests: ok");
    return 0;
}
