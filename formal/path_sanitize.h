/**
 * @file path_sanitize.h
 * @brief Pure-C path sanitization matching mutar::sanitize_path (src/mutar.cpp).
 *
 * Used by CBMC harnesses and agreement unit tests. Keep rules in lockstep
 * with the C++ implementation in mutar.cpp.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef MUTAR_PATH_SANITIZE_H
#define MUTAR_PATH_SANITIZE_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Bounds are tight under CBMC (state space); larger for native unit tests.
 * Security properties do not depend on the larger native limits.
 */
#ifdef __CPROVER__
#define MUTAR_PATH_MAX 32
#define MUTAR_PATH_COMPONENTS 8
#else
#define MUTAR_PATH_MAX 256
#define MUTAR_PATH_COMPONENTS 64
#endif

/**
 * Sanitize an archive member path (mirrors mutar::sanitize_path).
 *
 * When @p absolute_names is false:
 *   - strip leading '/' characters
 *   - drop empty and "." components
 *   - ".." pops the previous component (discarded at root)
 *   - empty result becomes "."
 *
 * When @p absolute_names is true: copy @p path unchanged (truncated to fit).
 *
 * @param path            NUL-terminated input path (must not be NULL)
 * @param absolute_names  if true, do not strip or collapse
 * @param out             output buffer
 * @param out_size        size of @p out (must be >= 2)
 * @return 0 on success, -1 if the result would not fit in @p out
 */
int mutar_sanitize_path(const char *path, bool absolute_names,
                        char *out, size_t out_size);

#endif /* MUTAR_PATH_SANITIZE_H */
