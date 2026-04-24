/*
 * kbfs - Knowledge Base File System
 * Utility functions
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <sys/stat.h>
#include "mdkb.h"

/* ============================================================================
 * Memory Utilities
 * ============================================================================ */

void *kb_malloc(size_t size) {
    void *p = malloc(size);
    if (!p && size > 0) {
        fprintf(stderr, "Error: Memory allocation failed\n");
        exit(1);
    }
    return p;
}

void *kb_realloc(void *ptr, size_t size) {
    void *p = realloc(ptr, size);
    if (!p && size > 0) {
        fprintf(stderr, "Error: Memory reallocation failed\n");
        exit(1);
    }
    return p;
}

void *kb_calloc(size_t n, size_t size) {
    void *p = calloc(n, size);
    if (!p && n > 0 && size > 0) {
        fprintf(stderr, "Error: Calloc failed\n");
        exit(1);
    }
    return p;
}

void kb_free(void *ptr) {
    free(ptr);
}

/* ============================================================================
 * String Utilities
 * ============================================================================ */

char *kb_strdup(const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s);
    char *dup = kb_malloc(len + 1);
    memcpy(dup, s, len + 1);
    return dup;
}

char *kb_strndup(const char *s, size_t n) {
    if (!s) return NULL;
    size_t len = strlen(s);
    if (len > n) len = n;
    char *dup = kb_malloc(len + 1);
    memcpy(dup, s, len);
    dup[len] = '\0';
    return dup;
}

char *kb_str_tolower(char *s) {
    if (!s) return NULL;
    for (char *p = s; *p; p++) {
        *p = (char)tolower((unsigned char)*p);
    }
    return s;
}

int kb_str_starts_with(const char *s, const char *prefix) {
    if (!s || !prefix) return 0;
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

int kb_str_ends_with(const char *s, const char *suffix) {
    if (!s || !suffix) return 0;
    size_t s_len = strlen(s);
    size_t suf_len = strlen(suffix);
    if (s_len < suf_len) return 0;
    return strcmp(s + s_len - suf_len, suffix) == 0;
}

/* ============================================================================
 * Path Utilities
 * ============================================================================ */

char *kb_expand_tilde(const char *path) {
    if (!path) return NULL;
    if (path[0] != '~') return kb_strdup(path);

    const char *home = getenv("HOME");
    if (!home) {
        fprintf(stderr, "Error: HOME environment variable not set\n");
        return NULL;
    }

    size_t home_len = strlen(home);
    size_t path_len = strlen(path);
    char *expanded = kb_malloc(home_len + path_len);

    memcpy(expanded, home, home_len);
    memcpy(expanded + home_len, path + 1, path_len);  /* Skip ~ */

    return expanded;
}

char *kb_path_join(const char *base, const char *rel) {
    if (!base) return kb_strdup(rel);
    if (!rel) return kb_strdup(base);

    size_t base_len = strlen(base);
    size_t rel_len = strlen(rel);

    /* Remove trailing slash from base */
    while (base_len > 0 && base[base_len - 1] == '/') {
        base_len--;
    }

    char *joined = kb_malloc(base_len + 1 + rel_len + 1);
    memcpy(joined, base, base_len);
    joined[base_len] = '/';
    memcpy(joined + base_len + 1, rel, rel_len + 1);

    return joined;
}

/* ============================================================================
 * Time Utilities
 * ============================================================================ */

time_t kb_parse_timestamp(const char *str) {
    if (!str || !*str) return 0;

    /* Try ISO 8601 format: YYYY-MM-DDTHH:MM:SS */
    struct tm tm = {0};
    if (sscanf(str, "%d-%d-%dT%d:%d:%d",
               &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
               &tm.tm_hour, &tm.tm_min, &tm.tm_sec) == 6) {
        tm.tm_year -= 1900;
        tm.tm_mon -= 1;
        return mktime(&tm);
    }

    /* Try simple date: YYYY-MM-DD */
    if (sscanf(str, "%d-%d-%d",
               &tm.tm_year, &tm.tm_mon, &tm.tm_mday) == 3) {
        tm.tm_year -= 1900;
        tm.tm_mon -= 1;
        return mktime(&tm);
    }

    /* Try Unix timestamp */
    char *end;
    long ts = strtol(str, &end, 10);
    if (*end == '\0') return (time_t)ts;

    return 0;
}

char *kb_format_time(time_t t, char *buf, size_t len) {
    struct tm *tm = localtime(&t);
    if (!tm) {
        strncpy(buf, "Unknown", len);
        return buf;
    }
    strftime(buf, len, "%Y-%m-%d %H:%M", tm);
    return buf;
}
