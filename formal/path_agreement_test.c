/**
 * @file path_agreement_test.c
 * @brief Fixture tests for mutar_sanitize_path (mirrors mutar.cpp rules).
 *
 * Run under make verify. Fixtures encode the same algorithm as
 * mutar::sanitize_path in src/mutar.cpp — keep both in sync.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "path_sanitize.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void expect_eq(const char *path, bool absolute_names, const char *want)
{
    char out[MUTAR_PATH_MAX];
    int rc = mutar_sanitize_path(path, absolute_names, out, sizeof out);
    if (rc != 0) {
        fprintf(stderr, "FAIL: sanitize(%s, abs=%d) returned %d\n",
                path, (int)absolute_names, rc);
        failures++;
        return;
    }
    if (strcmp(out, want) != 0) {
        fprintf(stderr, "FAIL: sanitize(%s, abs=%d) => \"%s\" want \"%s\"\n",
                path, (int)absolute_names, out, want);
        failures++;
        return;
    }
    printf("  ok  sanitize(%s, abs=%d) => \"%s\"\n", path, (int)absolute_names, out);
}

int main(void)
{
    failures = 0;
    printf("formal path_agreement_test fixtures:\n");

    /* Traversal / absolute stripping (absolute_names = false) */
    expect_eq("../../evil.txt", false, "evil.txt");
    expect_eq("../evil", false, "evil");
    expect_eq("/etc/passwd", false, "etc/passwd");
    expect_eq("///a/b", false, "a/b");
    expect_eq("a/b/../c", false, "a/c");
    expect_eq("a/./b", false, "a/b");
    expect_eq("a//b", false, "a/b");
    expect_eq("..", false, ".");
    expect_eq("../", false, ".");
    expect_eq(".", false, ".");
    expect_eq("./", false, ".");
    expect_eq("", false, ".");
    expect_eq("foo/bar", false, "foo/bar");
    expect_eq("foo/../../x", false, "x");
    expect_eq("a/b/c/../../d", false, "a/d");

    /* absolute_names = true: pass-through */
    expect_eq("/etc/passwd", true, "/etc/passwd");
    expect_eq("../../evil.txt", true, "../../evil.txt");
    expect_eq("a/b", true, "a/b");

    if (failures != 0) {
        fprintf(stderr, "path_agreement_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("path_agreement_test: all fixtures passed\n");
    return 0;
}
