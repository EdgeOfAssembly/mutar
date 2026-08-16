/**
 * @file path_sanitize.c
 * @brief C23 reimplementation of mutar::sanitize_path for formal verification.
 *
 * Algorithm must stay identical to the C++ version in src/mutar.cpp
 * (strip leading '/', collapse "." / "..", empty → ".").
 *
 * Implementation avoids large 2D stack arrays so CBMC stays tractable.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "path_sanitize.h"

#include <string.h>

int mutar_sanitize_path(const char *path, bool absolute_names,
                        char *out, size_t out_size)
{
    size_t plen;
    size_t i;
    size_t start;
    size_t out_len;

    if (path == NULL || out == NULL || out_size < 2) {
        return -1;
    }

    plen = strlen(path);

    if (absolute_names) {
        if (plen + 1 > out_size) {
            return -1;
        }
        for (i = 0; i <= plen; i++) {
            out[i] = path[i];
        }
        return 0;
    }

    /* Strip leading '/' */
    i = 0;
    while (i < plen && path[i] == '/') {
        i++;
    }

    out_len = 0;
    start = i;

    while (start <= plen) {
        size_t end = start;
        size_t part_len;
        int is_dot;
        int is_dotdot;

        while (end < plen && path[end] != '/') {
            end++;
        }
        part_len = end - start;

        is_dot = (part_len == 1 && path[start] == '.');
        is_dotdot = (part_len == 2 && path[start] == '.' && path[start + 1] == '.');

        if (part_len > 0 && !is_dot) {
            if (is_dotdot) {
                /* Pop last component from out */
                if (out_len == 0) {
                    /* at root: discard */
                } else {
                    /* remove trailing "/comp" or "comp" */
                    if (out_len > 0 && out[out_len - 1] == '/') {
                        out_len--;
                    }
                    while (out_len > 0 && out[out_len - 1] != '/') {
                        out_len--;
                    }
                    /* drop trailing slash left after pop (except keep empty) */
                    if (out_len > 0 && out[out_len - 1] == '/') {
                        out_len--;
                    }
                }
            } else {
                /* Append component */
                size_t need = part_len + (out_len > 0 ? 1 : 0);
                size_t j;
                if (out_len + need + 1 > out_size) {
                    return -1;
                }
                if (out_len > 0) {
                    out[out_len++] = '/';
                }
                for (j = 0; j < part_len; j++) {
                    out[out_len++] = path[start + j];
                }
            }
        }

        if (end >= plen) {
            break;
        }
        start = end + 1;
    }

    if (out_len == 0) {
        out[0] = '.';
        out[1] = '\0';
        return 0;
    }
    out[out_len] = '\0';
    return 0;
}
