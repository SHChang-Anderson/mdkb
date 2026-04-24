/*
 * kbfs - Knowledge Base File System
 * Search index operations
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mdkb.h"

/* Default hash table size */
#define DEFAULT_HT_SIZE 65536

/* Global ID counter */
static uint64_t g_next_entry_id = 1;

/* Create new index */
KB_Index *mdkb_index_new(void) {
    KB_Index *index = kb_calloc(1, sizeof(KB_Index));
    index->word_index = ht_new(DEFAULT_HT_SIZE);
    index->path_to_id = ht_new(4096);
    index->entries = NULL;
    index->entry_count = 0;
    index->entry_capacity = 0;
    index->total_terms = 0;
    index->avg_doc_length = 0.0f;
    return index;
}

/* Free index */
void mdkb_index_free(KB_Index *index) {
    if (!index) return;

    /* Free posting lists */
    ht_free(index->word_index, (void (*)(void *))posting_list_free);

    /* Free path to ID mapping (values are just IDs, no allocation) */
    ht_free(index->path_to_id, NULL);

    /* Free entries */
    if (index->entries) {
        for (size_t i = 0; i < index->entry_count; i++) {
            mdkb_entry_free(&index->entries[i]);
        }
        free(index->entries);
    }

    free(index);
}

/* Add entry to index (deep copy) */
int mdkb_index_add_entry(KB_Index *index, KB_Entry *entry) {
    if (!index || !entry) return -1;

    /* Assign ID if not set */
    if (entry->id == 0) {
        entry->id = g_next_entry_id++;
    }

    /* Grow array if needed */
    if (index->entry_count >= index->entry_capacity) {
        size_t new_capacity = index->entry_capacity == 0 ? 1024 : index->entry_capacity * 2;
        index->entries = kb_realloc(index->entries, new_capacity * sizeof(KB_Entry));
        index->entry_capacity = new_capacity;
    }

    /* Deep copy entry */
    KB_Entry *dest = &index->entries[index->entry_count];
    dest->id = entry->id;
    dest->path = entry->path ? kb_strdup(entry->path) : NULL;
    dest->title = entry->title ? kb_strdup(entry->title) : NULL;
    dest->content = entry->content ? kb_strdup(entry->content) : NULL;
    dest->raw_content = entry->raw_content ? kb_strdup(entry->raw_content) : NULL;
    dest->timestamp = entry->timestamp;
    dest->content_len = entry->content_len;
    dest->raw_len = entry->raw_len;
    dest->session_id = entry->session_id ? kb_strdup(entry->session_id) : NULL;
    dest->status = entry->status ? kb_strdup(entry->status) : NULL;

    /* Deep copy tags */
    dest->tag_count = entry->tag_count;
    if (entry->tag_count > 0 && entry->tags) {
        dest->tags = kb_malloc(entry->tag_count * sizeof(char *));
        for (size_t i = 0; i < entry->tag_count; i++) {
            dest->tags[i] = entry->tags[i] ? kb_strdup(entry->tags[i]) : NULL;
        }
    } else {
        dest->tags = NULL;
        dest->tag_count = 0;
    }

    /* Add path to ID mapping */
    if (entry->path) {
        uint64_t *id_ptr = kb_malloc(sizeof(uint64_t));
        *id_ptr = entry->id;
        ht_insert(index->path_to_id, entry->path, id_ptr);
    }

    index->entry_count++;
    return 0;
}

/* Get entry by ID */
KB_Entry *mdkb_index_get_entry(KB_Index *index, uint64_t id) {
    if (!index || id == 0) return NULL;

    /* Linear search for now (can optimize with binary search if sorted) */
    for (size_t i = 0; i < index->entry_count; i++) {
        if (index->entries[i].id == id) {
            return &index->entries[i];
        }
    }
    return NULL;
}

/* Find entry by path */
KB_Entry *mdkb_index_find_by_path(KB_Index *index, const char *path) {
    if (!index || !path) return NULL;

    uint64_t *id = (uint64_t *)ht_get(index->path_to_id, path);
    if (!id) return NULL;

    return mdkb_index_get_entry(index, *id);
}

/* Sort entries by timestamp descending (newest first) */
static int cmp_entry_timestamp_desc(const void *a, const void *b) {
    const KB_Entry *ea = (const KB_Entry *)a;
    const KB_Entry *eb = (const KB_Entry *)b;
    if (eb->timestamp > ea->timestamp) return  1;
    if (eb->timestamp < ea->timestamp) return -1;
    return 0;
}

void mdkb_index_sort_by_time(KB_Index *index) {
    if (!index || index->entry_count < 2) return;
    qsort(index->entries, index->entry_count, sizeof(KB_Entry), cmp_entry_timestamp_desc);
}

static int cmp_entry_path_asc(const void *a, const void *b) {
    const KB_Entry *ea = (const KB_Entry *)a;
    const KB_Entry *eb = (const KB_Entry *)b;
    const char *pa = ea->path ? ea->path : "";
    const char *pb = eb->path ? eb->path : "";
    return strcmp(pa, pb);
}

void mdkb_index_sort_by_path(KB_Index *index) {
    if (!index || index->entry_count < 2) return;
    qsort(index->entries, index->entry_count, sizeof(KB_Entry), cmp_entry_path_asc);
}

/* Calculate average document length */
void mdkb_index_calc_avg_length(KB_Index *index) {
    if (!index || index->entry_count == 0) return;

    size_t total_len = 0;
    for (size_t i = 0; i < index->entry_count; i++) {
        total_len += index->entries[i].content_len;
    }

    index->avg_doc_length = (float)total_len / index->entry_count;
}

/* Free posting list */
void posting_list_free(PostingList *list) {
    if (!list) return;

    PostingNode *node = list->head;
    while (node) {
        PostingNode *next = node->next;
        free(node);
        node = next;
    }

    free(list);
}

/* Create new posting list */
PostingList *posting_list_new(void) {
    return kb_calloc(1, sizeof(PostingList));
}

/* Add posting to list.
 *
 * Contract: callers MUST process one entry_id to completion before moving to
 * another. The fast path relies on the invariant that, within a single entry,
 * any prior add for this word sits at list->head (nodes are prepended). This
 * turns index build from O(N^2) to O(N). Breaking this contract (e.g. interleaved
 * entries, multi-threaded adds, incremental re-adds) will produce duplicate
 * nodes and corrupt doc_count. */
void posting_list_add(PostingList *list, uint64_t entry_id, int freq) {
    if (!list) return;

    if (list->head && list->head->entry_id == entry_id) {
        list->head->frequency += freq;
        return;
    }

    PostingNode *node = kb_malloc(sizeof(PostingNode));
    node->entry_id = entry_id;
    node->frequency = freq;
    node->tf_weight = 0.0f;
    node->next = list->head;
    list->head = node;
    list->doc_count++;
}
