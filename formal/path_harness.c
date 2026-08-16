/**
 * @file path_harness.c
 * @brief CBMC harness for mutar_sanitize_path security properties.
 *
 * Proves (bounded):
 *   - when absolute_names is false, result never starts with '/'
 *   - when absolute_names is false, no ".." path component remains
 *   - result is always non-empty on success
 *   - concrete fixtures match mutar.cpp rules
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "path_sanitize.h"

#include <stdbool.h>
#include <string.h>

#ifdef __CPROVER__
unsigned nondet_unsigned(void);
bool nondet_bool(void);
#else
#define __CPROVER_assume(x) ((void)(x))
#define __CPROVER_assert(c, m) \
    do {                       \
        if (!(c)) {            \
            return;            \
        }                      \
    } while (0)
static unsigned nondet_unsigned(void) { return 0; }
static bool nondet_bool(void) { return false; }
#endif

/** True if @p s has a ".." component (split on '/'). */
static int has_dotdot_component(const char *s)
{
    size_t i = 0;
    size_t n = strlen(s);

    while (i <= n) {
        size_t end = i;
        while (end < n && s[end] != '/') {
            end++;
        }
        if (end - i == 2 && s[i] == '.' && s[i + 1] == '.') {
            return 1;
        }
        if (end >= n) {
            break;
        }
        i = end + 1;
    }
    return 0;
}

/**
 * CBMC entry: short nondet paths over alphabet { '/', '.', 'a' }.
 * Length bound N=5 keeps BMC tractable with reduced MUTAR_PATH_* under CBMC.
 */
void harness(void)
{
    enum { N = 5 };
    char path[N];
    char out[MUTAR_PATH_MAX];
    bool absolute_names = false; /* security properties only for strip mode */
    unsigned len;
    int rc;
    unsigned i;

    len = nondet_unsigned();
    __CPROVER_assume(len < (unsigned)N);

    for (i = 0; i < (unsigned)N - 1u; i++) {
        if (i < len) {
            unsigned pick = nondet_unsigned();
            __CPROVER_assume(pick < 3u);
            path[i] = (pick == 0u) ? '/' : (pick == 1u) ? '.' : 'a';
        } else {
            path[i] = '\0';
        }
    }
    path[N - 1] = '\0';
    path[len] = '\0';

    /* absolute_names fixed false — prove strip/collapse safety */
    (void)absolute_names;
    memset(out, 0, sizeof out);

    rc = mutar_sanitize_path(path, false, out, sizeof out);

    if (rc == 0) {
        __CPROVER_assert(strlen(out) < sizeof out, "out NUL-terminated in bounds");
        __CPROVER_assert(out[0] != '/', "no leading slash when !absolute_names");
        __CPROVER_assert(!has_dotdot_component(out),
                         "no .. component when !absolute_names");
        __CPROVER_assert(out[0] != '\0', "non-empty result");
    }
}

/**
 * Concrete fixture checks (same rules as mutar::sanitize_path).
 */
void harness_fixtures(void)
{
    char out[MUTAR_PATH_MAX];
    int rc;

    rc = mutar_sanitize_path("../../evil.txt", false, out, sizeof out);
    __CPROVER_assert(rc == 0, "fixture trav ok");
    __CPROVER_assert(strcmp(out, "evil.txt") == 0, "fixture trav result");

    rc = mutar_sanitize_path("/etc/passwd", false, out, sizeof out);
    __CPROVER_assert(rc == 0, "fixture abs strip ok");
    __CPROVER_assert(strcmp(out, "etc/passwd") == 0, "fixture abs strip result");

    rc = mutar_sanitize_path("/etc/passwd", true, out, sizeof out);
    __CPROVER_assert(rc == 0, "fixture abs keep ok");
    __CPROVER_assert(strcmp(out, "/etc/passwd") == 0, "fixture abs keep result");

    rc = mutar_sanitize_path("a/b/../c", false, out, sizeof out);
    __CPROVER_assert(rc == 0, "fixture collapse ok");
    __CPROVER_assert(strcmp(out, "a/c") == 0, "fixture collapse result");

    rc = mutar_sanitize_path("..", false, out, sizeof out);
    __CPROVER_assert(rc == 0, "fixture root-dotdot ok");
    __CPROVER_assert(strcmp(out, ".") == 0, "fixture root-dotdot result");

    rc = mutar_sanitize_path("", false, out, sizeof out);
    __CPROVER_assert(rc == 0, "fixture empty ok");
    __CPROVER_assert(strcmp(out, ".") == 0, "fixture empty result");
}
