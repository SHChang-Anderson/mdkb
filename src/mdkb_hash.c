/*
 * kbfs - Knowledge Base File System
 * Hash table implementation for inverted index
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mdkb.h"

/* FNV-1a hash function */
uint32_t ht_hash(const char *key) {
    uint32_t hash = 2166136261U;
    for (const char *p = key; *p; p++) {
        hash ^= (unsigned char)*p;
        hash *= 16777619;
    }
    return hash;
}

/* Create new hash table */
HashTable *ht_new(size_t size) {
    HashTable *ht = kb_calloc(1, sizeof(HashTable));
    ht->size = size;
    ht->buckets = kb_calloc(size, sizeof(HT_Node *));
    ht->count = 0;
    return ht;
}

/* Free hash table */
void ht_free(HashTable *ht, void (*free_value)(void *)) {
    if (!ht) return;

    for (size_t i = 0; i < ht->size; i++) {
        HT_Node *node = ht->buckets[i];
        while (node) {
            HT_Node *next = node->next;
            free(node->key);
            if (free_value) {
                free_value(node->value);
            }
            free(node);
            node = next;
        }
    }

    free(ht->buckets);
    free(ht);
}

/* Insert key-value pair */
void ht_insert(HashTable *ht, const char *key, void *value) {
    if (!ht || !key) return;

    uint32_t hash = ht_hash(key);
    size_t idx = hash % ht->size;

    /* Check if key exists */
    HT_Node *node = ht->buckets[idx];
    while (node) {
        if (strcmp(node->key, key) == 0) {
            /* Update existing value */
            node->value = value;
            return;
        }
        node = node->next;
    }

    /* Create new node */
    node = kb_malloc(sizeof(HT_Node));
    node->key = kb_strdup(key);
    node->value = value;
    node->next = ht->buckets[idx];
    ht->buckets[idx] = node;
    ht->count++;
}

/* Get value by key */
void *ht_get(HashTable *ht, const char *key) {
    if (!ht || !key) return NULL;

    uint32_t hash = ht_hash(key);
    size_t idx = hash % ht->size;

    HT_Node *node = ht->buckets[idx];
    while (node) {
        if (strcmp(node->key, key) == 0) {
            return node->value;
        }
        node = node->next;
    }

    return NULL;
}

/* Remove key from hash table */
void ht_remove(HashTable *ht, const char *key) {
    if (!ht || !key) return;

    uint32_t hash = ht_hash(key);
    size_t idx = hash % ht->size;

    HT_Node **current = &ht->buckets[idx];
    while (*current) {
        if (strcmp((*current)->key, key) == 0) {
            HT_Node *to_remove = *current;
            *current = (*current)->next;
            free(to_remove->key);
            free(to_remove);
            ht->count--;
            return;
        }
        current = &(*current)->next;
    }
}
