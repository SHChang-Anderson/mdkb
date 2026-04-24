/*
 * mdkb - Markdown Knowledge Base
 * Core type definitions and data structures
 */

#ifndef MDKB_H
#define MDKB_H

#include <stdint.h>
#include <stddef.h>
#include <time.h>

/* Version info */
#define MDKB_VERSION "0.1.0"
#define MDKB_NAME "mdkb"

/* Default paths */
#define MDKB_DIR "~/.mdkb"
#define MDKB_NOTES_DIR "notes"
#define MDKB_ARCHIVE_DIR "archive"

/* Limits */
#define MDKB_MAX_TITLE_LEN 256
#define MDKB_MAX_TAG_COUNT 32
#define MDKB_MAX_TAG_LEN 64
#define MDKB_MAX_PATH_LEN 4096

/* ============================================================================
 * Knowledge Entry
 * ============================================================================ */

typedef struct {
    uint64_t id;                 /* Unique entry ID */
    char *path;                  /* Relative path from KBFS root */
    char *title;                 /* Entry title */
    char *content;               /* Full content (without front-matter) */
    char *raw_content;           /* Raw file content (for display) */
    time_t timestamp;            /* Creation/modification time */
    char **tags;                 /* Tag array */
    size_t tag_count;            /* Number of tags */
    size_t content_len;          /* Content length */
    size_t raw_len;              /* Raw content length */
    char *session_id;            /* Claude Code session ID (from YAML) */
    char *status;                /* Note status: draft, verified, stale */
} KB_Entry;

/* Create/destroy entries */
KB_Entry *mdkb_entry_new(void);
void mdkb_entry_free(KB_Entry *entry);

/* ============================================================================
 * Hash Table (for inverted index)
 * ============================================================================ */

typedef struct HT_Node {
    char *key;
    void *value;
    struct HT_Node *next;
} HT_Node;

typedef struct {
    HT_Node **buckets;
    size_t size;
    size_t count;
} HashTable;

/* Hash table operations */
HashTable *ht_new(size_t size);
void ht_free(HashTable *ht, void (*free_value)(void *));
void ht_insert(HashTable *ht, const char *key, void *value);
void *ht_get(HashTable *ht, const char *key);
void ht_remove(HashTable *ht, const char *key);
uint32_t ht_hash(const char *key);

/* ============================================================================
 * Posting List (for inverted index)
 * ============================================================================ */

typedef struct PostingNode {
    uint64_t entry_id;           /* Entry ID */
    int frequency;               /* Term frequency in this entry */
    float tf_weight;             /* TF component of BM25 */
    struct PostingNode *next;
} PostingNode;

typedef struct {
    PostingNode *head;
    size_t doc_count;            /* Number of documents containing this term */
    float idf;                   /* IDF component of BM25 */
} PostingList;

PostingList *posting_list_new(void);
void posting_list_free(PostingList *list);
void posting_list_add(PostingList *list, uint64_t entry_id, int freq);

/* ============================================================================
 * Search Index
 * ============================================================================ */

typedef struct {
    HashTable *word_index;       /* Inverted index: word -> PostingList */
    HashTable *path_to_id;       /* Path to entry ID mapping */
    KB_Entry *entries;           /* Array of all entries */
    size_t entry_count;
    size_t entry_capacity;
    size_t total_terms;          /* Total terms across all entries */
    float avg_doc_length;        /* Average document length (for BM25) */
} KB_Index;

/* Index operations */
KB_Index *mdkb_index_new(void);
void mdkb_index_free(KB_Index *index);
int mdkb_index_add_entry(KB_Index *index, KB_Entry *entry);
KB_Entry *mdkb_index_get_entry(KB_Index *index, uint64_t id);
KB_Entry *mdkb_index_find_by_path(KB_Index *index, const char *path);
void mdkb_index_calc_avg_length(KB_Index *index);
void mdkb_index_sort_by_time(KB_Index *index);
void mdkb_index_sort_by_path(KB_Index *index);

/* Posting list operations */
PostingList *posting_list_new(void);
void posting_list_free(PostingList *list);
void posting_list_add(PostingList *list, uint64_t entry_id, int freq);

/* ============================================================================
 * Search
 * ============================================================================ */

typedef struct {
    uint64_t entry_id;
    float score;
    float title_score;
    float tag_score;
    float content_score;
} SearchResult;

typedef struct {
    SearchResult *results;
    size_t count;
    size_t capacity;
} SearchResults;

/* BM25 parameters */
#define BM25_K1 1.2f
#define BM25_B 0.75f

/* Search operations */
SearchResults *mdkb_search(KB_Index *index, const char *query);
SearchResults *mdkb_search_weighted(KB_Index *index, const char *query,
                                   int title_weight, int tag_weight, int content_weight);
void mdmdkb_search_free(SearchResults *results);

/* Tokenization */
char **kb_tokenize(const char *text, size_t *token_count);
void kb_tokens_free(char **tokens, size_t count);

/* ============================================================================
 * YAML Front-matter Parser
 * ============================================================================ */

typedef struct {
    char *title;
    char **tags;
    size_t tag_count;
    time_t timestamp;
    char *type;
    char *session_id;            /* Claude Code session ID */
    char *cwd;                   /* Working directory at time of conversation */
    char *status;                /* Note status: draft, verified, stale */
    char *repo;                  /* Repository name (from frontmatter) */
    char *topic;                 /* Topic name (from frontmatter) */
} FrontMatter;

FrontMatter *mdkb_yaml_parse(const char *content, size_t len);
void mdkb_yaml_free(FrontMatter *fm);
char *mdkb_yaml_extract_content(const char *raw_content, size_t *content_len);

/* ============================================================================
 * File System Operations
 * ============================================================================ */

/* Scan directory for markdown files */
int mdkb_fs_scan(KB_Index *index, const char *mdkb_root);
int mdkb_fs_scan_recursive(KB_Index *index, const char *dir, const char *mdkb_root);
int mdkb_fs_load_file(KB_Index *index, const char *path, const char *mdkb_root);

/* ============================================================================
 * TUI
 * ============================================================================ */

/* Initialize/destroy TUI */
int mdkb_tui_init(void);
void mdkb_tui_cleanup(void);

/* Run TUI main loop */
int mdkb_tui_run(KB_Index *index, const char *mdkb_root);

/* TUI modes */
typedef enum {
    TUI_MODE_LIST,       /* List view (dual pane) */
    TUI_MODE_TIMELINE,   /* Timeline view */
    TUI_MODE_SEARCH      /* Search mode */
} TUI_Mode;

/* ============================================================================
 * Ingest (Claude Code import)
 * ============================================================================ */

/* Import from Claude Code conversation */
int mdkb_ingest_claude(KB_Index *index, const char *json_path, const char *output_dir);

/* ============================================================================
 * CLI
 * ============================================================================ */

/* CLI modes */
typedef enum {
    CLI_MODE_TUI,        /* Interactive TUI */
    CLI_MODE_PICK,       /* TUI with pick mode (L key prints path and exits) */
    CLI_MODE_QUERY,      /* Query mode for AI */
    CLI_MODE_LOAD,       /* Search and output top result content */
    CLI_MODE_LIST,       /* List recent entries as JSON (non-interactive) */
    CLI_MODE_INGEST,     /* Import mode */
    CLI_MODE_INDEX,      /* Rebuild index */
    CLI_MODE_VERSION     /* Show version */
} CLI_Mode;

/* Command line parsing */
typedef struct {
    CLI_Mode mode;
    char *query;
    char *ingest_path;
    char *mdkb_root;
    int limit;
    int title_weight;
    int tag_weight;
    int content_weight;
    int archive_mode;    /* 1 = scan archive/ instead of knowledge/ */
} CLI_Options;

int kb_cli_parse(int argc, char *argv[], CLI_Options *opts);
void kb_cli_usage(void);
int kb_cli_query(KB_Index *index, CLI_Options *opts);
int kb_cli_load(KB_Index *index, CLI_Options *opts, const char *mdkb_root);
int kb_cli_list(KB_Index *index, CLI_Options *opts);
int kb_cli_ingest(CLI_Options *opts);

/* ============================================================================
 * Utilities
 * ============================================================================ */

/* String utilities */
char *kb_strdup(const char *s);
char *kb_strndup(const char *s, size_t n);
char *kb_str_tolower(char *s);
int kb_str_starts_with(const char *s, const char *prefix);
int kb_str_ends_with(const char *s, const char *suffix);

/* Path utilities */
char *kb_expand_tilde(const char *path);
char *kb_path_join(const char *base, const char *rel);

/* Time utilities */
time_t kb_parse_timestamp(const char *str);
char *kb_format_time(time_t t, char *buf, size_t len);

/* Memory utilities */
void *kb_malloc(size_t size);
void *kb_realloc(void *ptr, size_t size);
void *kb_calloc(size_t n, size_t size);
void kb_free(void *ptr);

#endif /* MDKB_H */
