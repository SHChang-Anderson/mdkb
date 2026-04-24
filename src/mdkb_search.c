/*
 * kbfs - Knowledge Base File System
 * BM25 search implementation
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include "mdkb.h"

/* Initial token array capacity (grows dynamically) */
#define TOKEN_INIT_CAP 256

/* Return byte length of the UTF-8 character starting at c (1-4) */
static int utf8_char_len(unsigned char c) {
    if ((c & 0x80) == 0x00) return 1;  /* ASCII */
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;  /* CJK Unified Ideographs */
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;  /* continuation byte or invalid: treat as 1 */
}

/* Check if a UTF-8 character is in the CJK range */
static int is_cjk_char(const char *p) {
    unsigned char c = (unsigned char)*p;
    if (c < 0xE0) return 0;  /* CJK starts at U+4E00, encoded as 3-byte UTF-8 */
    if (utf8_char_len(c) < 3) return 0;
    /* Decode 3-byte UTF-8: 1110xxxx 10xxxxxx 10xxxxxx */
    unsigned int cp = ((c & 0x0F) << 12) |
                      (((unsigned char)p[1] & 0x3F) << 6) |
                      ((unsigned char)p[2] & 0x3F);
    /* CJK Unified Ideographs: U+4E00..U+9FFF */
    /* CJK Extension A: U+3400..U+4DBF */
    /* CJK Compatibility: U+F900..U+FAFF */
    /* Fullwidth punctuation: U+3000..U+303F */
    /* Hiragana/Katakana: U+3040..U+30FF */
    return (cp >= 0x3000 && cp <= 0x9FFF) || (cp >= 0xF900 && cp <= 0xFAFF);
}

/* Add a token to the dynamic array, growing if needed */
static void token_push(char ***tokens, size_t *count, size_t *cap, char *tok) {
    if (*count >= *cap) {
        *cap *= 2;
        *tokens = kb_realloc(*tokens, *cap * sizeof(char *));
    }
    (*tokens)[(*count)++] = tok;
}

/* Tokenize text into words (dynamic, no upper limit).
 * ASCII: split on word boundaries (letters/digits).
 * Non-ASCII (CJK): unigram + overlapping bigrams for precision.
 */
char **kb_tokenize(const char *text, size_t *token_count) {
    if (!text || !token_count) return NULL;

    size_t cap = TOKEN_INIT_CAP;
    char **tokens = kb_malloc(cap * sizeof(char *));
    *token_count = 0;

    const char *p = text;
    /* Track previous CJK char for bigram generation */
    const char *prev_cjk = NULL;
    int prev_cjk_len = 0;

    while (*p) {
        unsigned char c = (unsigned char)*p;

        if (c < 0x80) {
            /* ASCII path: skip non-alphanumeric */
            if (!isalnum(c)) { p++; prev_cjk = NULL; continue; }

            const char *start = p;
            while (*p && (isalnum((unsigned char)*p) || *p == '_')) p++;
            size_t len = (size_t)(p - start);
            if (len > 0 && len < 256) {
                char *tok = kb_strndup(start, len);
                kb_str_tolower(tok);
                token_push(&tokens, token_count, &cap, tok);
            }
            prev_cjk = NULL;
        } else {
            int clen = utf8_char_len(c);

            /* Unigram: single character token */
            token_push(&tokens, token_count, &cap, kb_strndup(p, (size_t)clen));

            /* Bigram: combine with previous CJK char for better CJK search */
            if (prev_cjk && is_cjk_char(p) && is_cjk_char(prev_cjk)) {
                size_t blen = (size_t)(prev_cjk_len + clen);
                char *bigram = kb_malloc(blen + 1);
                memcpy(bigram, prev_cjk, prev_cjk_len);
                memcpy(bigram + prev_cjk_len, p, clen);
                bigram[blen] = '\0';
                token_push(&tokens, token_count, &cap, bigram);
            }

            prev_cjk = p;
            prev_cjk_len = clen;
            p += clen;
        }
    }

    return tokens;
}

/* Free tokens */
void kb_tokens_free(char **tokens, size_t count) {
    if (!tokens) return;
    for (size_t i = 0; i < count; i++) {
        free(tokens[i]);
    }
    free(tokens);
}

/* Calculate BM25 score for a term */
static float calc_bm25_score(int term_freq, float doc_len, float avg_len,
                             float idf) {
    float k = BM25_K1;
    float b = BM25_B;

    float tf = term_freq * (k + 1) / (term_freq + k * (1 - b + b * doc_len / avg_len));
    return tf * idf;
}

/* Calculate IDF */
static float calc_idf(int total_docs, int doc_freq) {
    if (doc_freq == 0) return 0;
    return logf((float)total_docs / doc_freq);
}

/* Scored entry used during search ranking */
typedef struct {
    uint64_t entry_id;
    float title_score;
    float tag_score;
    float content_score;
    float total_score;
} ScoredEntry;

/* Comparison function for qsort: descending by total_score */
static int cmp_scored_entry_desc(const void *a, const void *b) {
    const ScoredEntry *sa = (const ScoredEntry *)a;
    const ScoredEntry *sb = (const ScoredEntry *)b;
    if (sb->total_score > sa->total_score) return 1;
    if (sb->total_score < sa->total_score) return -1;
    return 0;
}

/* Search with BM25 */
SearchResults *mdkb_search(KB_Index *index, const char *query) {
    return mdkb_search_weighted(index, query, 10, 5, 1);
}

/* Search with custom weights */
SearchResults *mdkb_search_weighted(KB_Index *index, const char *query,
                                   int title_weight, int tag_weight, int content_weight) {
    if (!index || !query || !*query) return NULL;

    /* Tokenize query */
    size_t query_token_count;
    char **query_tokens = kb_tokenize(query, &query_token_count);
    if (!query_tokens || query_token_count == 0) {
        kb_tokens_free(query_tokens, query_token_count);
        return NULL;
    }

    /* Calculate IDF for each query term */
    for (size_t i = 0; i < query_token_count; i++) {
        PostingList *list = ht_get(index->word_index, query_tokens[i]);
        if (list) {
            list->idf = calc_idf(index->entry_count, list->doc_count);
        }
    }

    /* Score entries */
    ScoredEntry *scored = kb_calloc(index->entry_count, sizeof(ScoredEntry));
    size_t scored_count = 0;

    for (size_t i = 0; i < index->entry_count; i++) {
        KB_Entry *entry = &index->entries[i];
        ScoredEntry *se = &scored[scored_count];
        se->entry_id = entry->id;
        se->title_score = 0;
        se->tag_score = 0;
        se->content_score = 0;

        /* Score title */
        if (entry->title) {
            size_t title_token_count;
            char **title_tokens = kb_tokenize(entry->title, &title_token_count);
            for (size_t qi = 0; qi < query_token_count; qi++) {
                for (size_t ti = 0; ti < title_token_count; ti++) {
                    if (strcmp(query_tokens[qi], title_tokens[ti]) == 0) {
                        se->title_score += title_weight;
                    }
                }
            }
            kb_tokens_free(title_tokens, title_token_count);
        }

        /* Score tags */
        for (size_t qi = 0; qi < query_token_count; qi++) {
            for (size_t ti = 0; ti < entry->tag_count; ti++) {
                if (strstr(entry->tags[ti], query_tokens[qi])) {
                    se->tag_score += tag_weight;
                }
            }
        }

        /* Score content with BM25 */
        if (entry->content) {
            for (size_t qi = 0; qi < query_token_count; qi++) {
                PostingList *list = ht_get(index->word_index, query_tokens[qi]);
                if (list) {
                    PostingNode *node = list->head;
                    while (node) {
                        if (node->entry_id == entry->id) {
                            float bm25 = calc_bm25_score(
                                node->frequency,
                                entry->content_len,
                                index->avg_doc_length,
                                list->idf
                            );
                            se->content_score += bm25 * content_weight;
                        }
                        node = node->next;
                    }
                }
            }
        }

        se->total_score = se->title_score + se->tag_score + se->content_score;

        if (se->total_score > 0) {
            scored_count++;
        }
    }

    /* Substring fallback: if BM25 missed entries, do case-insensitive match in-place */
    {
        /* Build lowercase query for case-insensitive matching */
        size_t qlen = strlen(query);
        char *query_lower = kb_strndup(query, qlen);
        kb_str_tolower(query_lower);

        for (size_t i = 0; i < index->entry_count; i++) {
            KB_Entry *entry = &index->entries[i];

            /* Skip if already scored by BM25 */
            int already = 0;
            for (size_t j = 0; j < scored_count; j++) {
                if (scored[j].entry_id == entry->id && scored[j].total_score > 0) {
                    already = 1;
                    break;
                }
            }
            if (already) continue;

            /* Case-insensitive substring match without copying content */
            int found = 0;
            if (entry->content && entry->content_len >= qlen) {
                const char *hay = entry->content;
                size_t hay_len = entry->content_len;
                for (size_t h = 0; h + qlen <= hay_len; h++) {
                    int ok = 1;
                    for (size_t q = 0; q < qlen; q++) {
                        if (tolower((unsigned char)hay[h + q]) != (unsigned char)query_lower[q]) {
                            ok = 0;
                            break;
                        }
                    }
                    if (ok) { found = 1; break; }
                }
            }
            /* Also check title */
            if (!found && entry->title) {
                size_t tlen = strlen(entry->title);
                if (tlen >= qlen) {
                    const char *hay = entry->title;
                    for (size_t h = 0; h + qlen <= tlen; h++) {
                        int ok = 1;
                        for (size_t q = 0; q < qlen; q++) {
                            if (tolower((unsigned char)hay[h + q]) != (unsigned char)query_lower[q]) {
                                ok = 0;
                                break;
                            }
                        }
                        if (ok) { found = 1; break; }
                    }
                }
            }

            if (found) {
                ScoredEntry *se = &scored[scored_count];
                se->entry_id = entry->id;
                se->title_score = 0;
                se->tag_score = 0;
                se->content_score = 0.5f;  /* baseline score for substring match */
                se->total_score = 0.5f;
                scored_count++;
            }
        }
        free(query_lower);
    }

    /* Sort results by score (descending) using qsort */
    qsort(scored, scored_count, sizeof(ScoredEntry), cmp_scored_entry_desc);

    /* Create results */
    SearchResults *results = kb_malloc(sizeof(SearchResults));
    results->count = scored_count;
    results->capacity = scored_count;
    results->results = kb_calloc(scored_count, sizeof(SearchResult));

    for (size_t i = 0; i < scored_count; i++) {
        results->results[i].entry_id = scored[i].entry_id;
        results->results[i].score = scored[i].total_score;
        results->results[i].title_score = scored[i].title_score;
        results->results[i].tag_score = scored[i].tag_score;
        results->results[i].content_score = scored[i].content_score;
    }

    free(scored);
    kb_tokens_free(query_tokens, query_token_count);

    return results;
}

/* Free search results */
void mdmdkb_search_free(SearchResults *results) {
    if (!results) return;
    free(results->results);
    free(results);
}
