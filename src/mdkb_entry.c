/*
 * kbfs - Knowledge Base File System
 * Knowledge entry operations
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mdkb.h"

/* Global ID counter */
static uint64_t g_next_id = 1;

/* Create new entry */
KB_Entry *mdkb_entry_new(void) {
    KB_Entry *entry = kb_calloc(1, sizeof(KB_Entry));
    entry->id = g_next_id++;
    return entry;
}

/* Free entry (but not the entry structure itself) */
void mdkb_entry_free(KB_Entry *entry) {
    if (!entry) return;

    free(entry->path);
    entry->path = NULL;
    free(entry->title);
    entry->title = NULL;
    free(entry->content);
    entry->content = NULL;
    free(entry->raw_content);
    entry->raw_content = NULL;

    if (entry->tags) {
        for (size_t i = 0; i < entry->tag_count; i++) {
            free(entry->tags[i]);
            entry->tags[i] = NULL;
        }
        free(entry->tags);
        entry->tags = NULL;
    }
    entry->tag_count = 0;

    free(entry->session_id);
    entry->session_id = NULL;
    free(entry->status);
    entry->status = NULL;
}

/* Set entry tags from array */
void mdkb_entry_set_tags(KB_Entry *entry, const char **tags, size_t count) {
    if (!entry) return;

    /* Free existing tags */
    if (entry->tags) {
        for (size_t i = 0; i < entry->tag_count; i++) {
            free(entry->tags[i]);
        }
        free(entry->tags);
    }

    /* Copy new tags */
    entry->tag_count = count > MDKB_MAX_TAG_COUNT ? MDKB_MAX_TAG_COUNT : count;
    if (entry->tag_count > 0) {
        entry->tags = kb_malloc(entry->tag_count * sizeof(char *));
        for (size_t i = 0; i < entry->tag_count; i++) {
            entry->tags[i] = kb_strdup(tags[i]);
        }
    } else {
        entry->tags = NULL;
    }
}

/* Check if entry has tag */
int mdkb_entry_has_tag(KB_Entry *entry, const char *tag) {
    if (!entry || !tag) return 0;

    for (size_t i = 0; i < entry->tag_count; i++) {
        if (strcmp(entry->tags[i], tag) == 0) {
            return 1;
        }
    }
    return 0;
}
