/* SPDX-License-Identifier: MIT
 * Copyright 2026 the Desktop Commander C contributors
 *
 * edit_block: exact search/replace with an expected count, and a report of
 * the closest text when the search string is absent.
 */
#include "tools.h"
#include "util.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static bool has_crlf(const char *s)
{
    return strstr(s, "\r\n") != NULL;
}

/* Make a string use CRLF (crlf) or LF. */
static char *to_eol(const char *s, bool crlf)
{
    sbuf b;
    sb_init(&b);
    for (size_t i = 0; s[i]; i++) {
        if (s[i] == '\r' && crlf && s[i + 1] == '\n') {
            sb_putn(&b, "\r\n", 2);
            i++;
        } else if (s[i] == '\r') {
            continue; /* a lone CR goes away */
        } else if (s[i] == '\n' && crlf) {
            sb_putn(&b, "\r\n", 2);
        } else {
            sb_putc(&b, s[i]);
        }
    }
    return sb_take(&b);
}

/* Dice coefficient over character bigrams: 0..1, cheap and order blind. The
 * 4096 bigrams live on the stack, so a failed edit allocates nothing. */
static double similarity(const char *a, const char *b, size_t alen, size_t blen)
{
    if (alen < 2 || blen < 2) {
        if (alen == 0 && blen == 0)
            return 1.0;
        return (alen && blen) ? 1.0 : 0.0;
    }
    enum { CAP = 4096, TAKEN = 0x10000 };
    uint32_t hits[CAP];
    size_t used = 0;
    for (size_t i = 0; i + 1 < alen && used < CAP; i++)
        hits[used++] = ((uint32_t)(unsigned char)a[i] << 8) | (unsigned char)a[i + 1];

    size_t common = 0;
    for (size_t j = 0; j + 1 < blen; j++) {
        uint32_t g = ((uint32_t)(unsigned char)b[j] << 8) | (unsigned char)b[j + 1];
        for (size_t k = 0; k < used; k++) {
            if (hits[k] == g) {
                hits[k] = TAKEN; /* one hit per bigram instance */
                common++;
                break;
            }
        }
    }
    return (2.0 * (double)common) / (double)((alen - 1) + (blen - 1));
}

/* Find the window of `nlines` lines (or fewer at the end) closest to `needle`. */
static char *closest_window(const char *hay, const char *needle, size_t nlines,
                            double *score, size_t *start_line)
{
    size_t hay_len = strlen(hay), needle_len = strlen(needle);
    char *best = xstrdup("");
    double best_score = 0.0;
    size_t best_line = 0;

    size_t line = 0, pos = 0;
    while (pos < hay_len && line < 200000) {
        size_t p = pos;
        size_t n = 0;
        while (p < hay_len && n < nlines) {
            const char *nl = strchr(hay + p, '\n');
            if (!nl) {
                p = hay_len;
                break;
            }
            p = (size_t)(nl - hay) + 1;
            n++;
        }
        if (p == pos)
            break;
        double s = similarity(needle, hay + pos, needle_len, p - pos);
        if (s > best_score) {
            best_score = s;
            best_line = line;
            free(best);
            best = xmalloc(p - pos + 1);
            memcpy(best, hay + pos, p - pos);
            best[p - pos] = '\0';
        }
        const char *nl = strchr(hay + pos, '\n');
        if (!nl)
            break;
        pos = (size_t)(nl - hay) + 1;
        line++;
    }
    if (score)
        *score = best_score;
    if (start_line)
        *start_line = best_line;
    return best;
}

tool_result tool_edit_block(jdoc *d, jref a)
{
    const char *path = jstr_get(d, a, "file_path", NULL);
    if (!path || !*path)
        return result_error(d, "Missing file_path");

    jref oldv = jget(d, a, "old_string");
    jref newv = jget(d, a, "new_string");
    jref rangev = jget(d, a, "range");
    bool has_text = jis_str(d, oldv) && *jstr_of(d, oldv) && jis_str(d, newv);
    bool has_range = jis_str(d, rangev) && *jstr_of(d, rangev) && jget(d, a, "content") != JNULL;

    if (!has_text && !has_range)
        return result_error(d, "Provide (old_string + new_string) or (range + content)");
    if (!has_text)
        return result_error(d, "Range editing is only supported for spreadsheet files; "
                               "text files use old_string and new_string.");

    int64_t expected = jint_get(d, a, "expected_replacements", 1);
    if (expected < 1)
        expected = 1;

    char *abs = NULL;
    char *err = validate_path(path, &abs);
    if (err) {
        tool_result r = result_error(d, "%s", err);
        free(err);
        return r;
    }

    size_t len = 0;
    char *content = read_file_all(abs, &len, &err);
    if (!content) {
        tool_result r = result_error(d, "%s", err);
        free(err);
        free(abs);
        return r;
    }

    if (data_is_binary(content, len)) {
        free(content);
        free(abs);
        return result_error(d, "Cannot edit %s: the file is binary", path);
    }

    bool crlf = has_crlf(content);
    char *search = to_eol(jstr_of(d, oldv), crlf);
    char *replace = to_eol(jstr_of(d, newv), crlf);

    long long count = 0;
    char *updated = str_replace_all(content, search, replace, expected, &count);

    if (count == 0) {
        double score = 0.0;
        size_t start_line = 0;
        size_t nlines = count_lines(search);
        char *near = closest_window(content, search, nlines ? nlines : 1, &score, &start_line);
        sbuf b;
        sb_init(&b);
        if (score >= 0.7) {
            sb_printf(&b,
                      "Exact match not found in %s. Closest text is %.0f%% similar "
                      "(line %zu) and was NOT changed:\n\n%s",
                      path, score * 100.0, start_line + 1, near);
        } else {
            sb_printf(&b,
                      "Search text not found in %s. Nothing was changed. "
                      "The closest text is %.0f%% similar, below the 70%% match threshold.",
                      path, score * 100.0);
        }
        tool_result r = result_buf(d, &b);
        free(near);
        free(search);
        free(replace);
        free(content);
        free(abs);
        return r;
    }

    if (!updated) {
        char *msg = xasprintf("Expected %lld occurrence(s) but found %lld in %s. "
                              "Nothing was changed. Set expected_replacements to %lld "
                              "to replace all of them.",
                              (long long)expected, count, path, count);
        tool_result r = result_text(d, msg);
        free(msg);
        free(search);
        free(replace);
        free(content);
        free(abs);
        return r;
    }

    char *out = write_file_atomic(abs, updated, strlen(updated), false, 0644, &err);
    if (!out) {
        tool_result r = result_error(d, "%s", err);
        free(err);
        free(updated);
        free(search);
        free(replace);
        free(content);
        free(abs);
        return r;
    }
    free(out);

    /* A preview around the first change, as the original returns. */
    size_t total = count_lines(updated);
    size_t change_at = 0;
    const char *hit = strstr(updated, replace);
    if (hit && replace[0])
        change_at = count_lines_upto(updated, (size_t)(hit - updated));
    size_t first = change_at > 10 ? change_at - 10 : 0;
    size_t skip = first ? bytes_for_lines(updated, first) : 0;
    size_t preview_bytes = bytes_for_lines(updated + skip, 21);
    char *preview = xmalloc(preview_bytes + 1);
    memcpy(preview, updated + skip, preview_bytes);
    preview[preview_bytes] = '\0';
    size_t shown = count_lines(preview);
    size_t remaining = total > (first + shown) ? total - (first + shown) : 0;

    sbuf b;
    sb_init(&b);
    if (first == 0)
        sb_printf(&b, "[Reading %zu lines from start (total: %zu lines, %zu remaining)]\n\n",
                  shown, total, remaining);
    else
        sb_printf(&b, "[Reading %zu lines from line %zu (total: %zu lines, %zu remaining)]\n\n",
                  shown, first, total, remaining);
    sb_put(&b, preview);

    tool_result r = result_buf(d, &b);
    free(preview);
    free(updated);
    free(search);
    free(replace);
    free(content);
    free(abs);
    return r;
}
