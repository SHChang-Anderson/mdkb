/*
 * kbfs - Knowledge Base File System
 * YAML front-matter parser (simplified)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "mdkb.h"

/* Parse array like: ["tag1", "tag2"] */
static char **parse_yaml_array(const char *str, size_t *count) {
    *count = 0;
    if (!str) return NULL;

    char **tags = kb_malloc(MDKB_MAX_TAG_COUNT * sizeof(char *));

    /* Simple parser for ["item1", "item2"] format */
    const char *p = strchr(str, '[');
    if (!p) {
        free(tags);
        return NULL;
    }
    p++;

    while (*p && *p != ']') {
        /* Skip whitespace */
        while (*p && isspace((unsigned char)*p)) p++;

        /* Skip comma */
        if (*p == ',') {
            p++;
            continue;
        }

        /* Parse quoted string */
        if (*p == '"' || *p == '\'') {
            char quote = *p;
            p++;
            const char *start = p;
            while (*p && *p != quote) p++;

            if (*count < MDKB_MAX_TAG_COUNT) {
                tags[*count] = kb_strndup(start, p - start);
                (*count)++;
            }

            if (*p == quote) p++;
        } else {
            /* Parse unquoted word */
            const char *start = p;
            while (*p && *p != ',' && *p != ']' && !isspace((unsigned char)*p)) p++;

            if (*count < MDKB_MAX_TAG_COUNT) {
                tags[*count] = kb_strndup(start, p - start);
                (*count)++;
            }
        }
    }

    if (*count == 0) {
        free(tags);
        return NULL;
    }

    return tags;
}

/* Parse YAML front-matter from content */
FrontMatter *mdkb_yaml_parse(const char *content, size_t len) {
    if (!content || len < 10) return NULL;

    /* Check if starts with --- */
    if (strncmp(content, "---\n", 4) != 0 &&
        strncmp(content, "---\r\n", 5) != 0) {
        return NULL;
    }

    /* Find end of front-matter */
    const char *fm_end = strstr(content + 3, "---");
    if (!fm_end) return NULL;

    /* Allocate result */
    FrontMatter *fm = kb_calloc(1, sizeof(FrontMatter));

    /* Parse line by line */
    const char *line = content + 3;
    const char *end = fm_end;

    while (line < end && *line) {
        /* Skip empty lines */
        while (line < end && (*line == '\n' || *line == '\r')) line++;
        if (line >= end) break;

        /* Find end of line */
        const char *line_end = line;
        while (line_end < end && *line_end != '\n') line_end++;

        /* Parse key: value */
        const char *colon = strchr(line, ':');
        if (colon && colon < line_end) {
            /* Extract key */
            size_t key_len = colon - line;
            while (key_len > 0 && isspace((unsigned char)line[key_len - 1])) key_len--;

            /* Extract value */
            const char *value = colon + 1;
            while (value < line_end && isspace((unsigned char)*value)) value++;

            size_t value_len = line_end - value;
            while (value_len > 0 && isspace((unsigned char)value[value_len - 1])) value_len--;

            /* Remove quotes if present */
            if (value_len >= 2 &&
                ((value[0] == '"' && value[value_len - 1] == '"') ||
                 (value[0] == '\'' && value[value_len - 1] == '\''))) {
                value++;
                value_len -= 2;
            }

            /* Store values */
            if (key_len == 5 && strncmp(line, "title", key_len) == 0) {
                fm->title = kb_strndup(value, value_len);
            } else if (key_len == 4 && strncmp(line, "tags", key_len) == 0) {
                /* Parse as array */
                char *tag_str = kb_strndup(value, value_len);
                fm->tags = parse_yaml_array(tag_str, &fm->tag_count);
                free(tag_str);
            } else if (key_len == 4 && strncmp(line, "type", key_len) == 0) {
                fm->type = kb_strndup(value, value_len);
            } else if (key_len == 9 && strncmp(line, "timestamp", key_len) == 0) {
                char *ts_str = kb_strndup(value, value_len);
                fm->timestamp = kb_parse_timestamp(ts_str);
                free(ts_str);
            } else if (key_len == 4 && strncmp(line, "date", key_len) == 0) {
                char *date_str = kb_strndup(value, value_len);
                fm->timestamp = kb_parse_timestamp(date_str);
                free(date_str);
            } else if (key_len == 10 && strncmp(line, "session_id", key_len) == 0) {
                fm->session_id = kb_strndup(value, value_len);
            } else if (key_len == 3 && strncmp(line, "cwd", key_len) == 0) {
                fm->cwd = kb_strndup(value, value_len);
            } else if (key_len == 6 && strncmp(line, "status", key_len) == 0) {
                fm->status = kb_strndup(value, value_len);
            } else if (key_len == 4 && strncmp(line, "repo", key_len) == 0) {
                fm->repo = kb_strndup(value, value_len);
            } else if (key_len == 5 && strncmp(line, "topic", key_len) == 0) {
                fm->topic = kb_strndup(value, value_len);
            }
        }

        line = line_end + 1;
    }

    return fm;
}

/* Free front-matter */
void mdkb_yaml_free(FrontMatter *fm) {
    if (!fm) return;

    free(fm->title);
    free(fm->type);
    free(fm->session_id);
    free(fm->cwd);
    free(fm->status);
    free(fm->repo);
    free(fm->topic);

    if (fm->tags) {
        for (size_t i = 0; i < fm->tag_count; i++) {
            free(fm->tags[i]);
        }
        free(fm->tags);
    }

    free(fm);
}

/* Extract content after front-matter */
char *mdkb_yaml_extract_content(const char *raw_content, size_t *content_len) {
    if (!raw_content) {
        *content_len = 0;
        return NULL;
    }

    /* Check for front-matter */
    if (strncmp(raw_content, "---", 3) != 0) {
        *content_len = strlen(raw_content);
        return kb_strdup(raw_content);
    }

    /* Find end of front-matter */
    const char *fm_end = strstr(raw_content + 3, "---");
    if (!fm_end) {
        *content_len = strlen(raw_content);
        return kb_strdup(raw_content);
    }

    /* Skip --- and following newline */
    const char *content = fm_end + 3;
    if (*content == '\n') content++;
    if (*content == '\r') content++;

    *content_len = strlen(content);
    return kb_strdup(content);
}
