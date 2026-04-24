/*
 * kbfs - Knowledge Base File System
 * File system scanning
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include "mdkb.h"

/* Read entire file into memory */
static char *read_file(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "Warning: Cannot open %s: %s\n", path, strerror(errno));
        return NULL;
    }

    /* Get file size */
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }

    long size = ftell(fp);
    if (size < 0) {
        fclose(fp);
        return NULL;
    }

    if (fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return NULL;
    }

    /* Allocate and read */
    char *content = kb_malloc(size + 1);
    size_t read_size = fread(content, 1, size, fp);
    fclose(fp);

    if (read_size != (size_t)size) {
        free(content);
        return NULL;
    }

    content[size] = '\0';
    return content;
}

/* Index a single file */
int mdkb_fs_load_file(KB_Index *index, const char *full_path, const char *mdkb_root) {
    if (!index || !full_path) return -1;

    /* Skip if already indexed, unless file was modified on disk */
    {
        size_t rlen = strlen(mdkb_root);
        if (strncmp(full_path, mdkb_root, rlen) == 0) {
            const char *rel = full_path + rlen;
            while (*rel == '/') rel++;
            KB_Entry *existing = mdkb_index_find_by_path(index, rel);
            if (existing) {
                struct stat fst;
                if (stat(full_path, &fst) != 0 || fst.st_mtime <= existing->timestamp)
                    return 0;
                /* File updated on disk — reload content in place */
                char *new_raw = read_file(full_path);
                if (!new_raw) return 0;
                free(existing->raw_content);
                existing->raw_content = new_raw;
                existing->raw_len = strlen(new_raw);
                free(existing->content);
                existing->content = mdkb_yaml_extract_content(new_raw, &existing->content_len);
                FrontMatter *fm = mdkb_yaml_parse(new_raw, existing->raw_len);
                if (fm) {
                    if (fm->title) { free(existing->title); existing->title = kb_strdup(fm->title); }
                    if (fm->timestamp > 0) existing->timestamp = fm->timestamp;
                    free(existing->status);
                    existing->status = fm->status ? kb_strdup(fm->status) : NULL;
                    mdkb_yaml_free(fm);
                }
                if (existing->timestamp == 0) existing->timestamp = fst.st_mtime;
                return 0;
            }
        }
    }

    /* Read file */
    char *raw_content = read_file(full_path);
    if (!raw_content) return -1;

    /* Create entry */
    KB_Entry entry = {0};
    entry.raw_content = raw_content;
    entry.raw_len = strlen(raw_content);

    /* Extract relative path */
    size_t root_len = strlen(mdkb_root);
    if (strncmp(full_path, mdkb_root, root_len) == 0) {
        const char *rel = full_path + root_len;
        while (*rel == '/') rel++;
        entry.path = kb_strdup(rel);
    } else {
        entry.path = kb_strdup(full_path);
    }

    /* Extract file name as default title */
    const char *filename = strrchr(full_path, '/');
    if (filename) {
        filename++;
    } else {
        filename = full_path;
    }
    entry.title = kb_strdup(filename);

    /* Parse front-matter */
    FrontMatter *fm = mdkb_yaml_parse(raw_content, entry.raw_len);
    if (fm) {
        if (fm->title) {
            free(entry.title);
            entry.title = kb_strdup(fm->title);
        }
        if (fm->tags && fm->tag_count > 0) {
            entry.tags = fm->tags;
            entry.tag_count = fm->tag_count;
            fm->tags = NULL;  /* Transfer ownership */
        }
        if (fm->timestamp > 0) {
            entry.timestamp = fm->timestamp;
        }
        if (fm->session_id) {
            entry.session_id = kb_strdup(fm->session_id);
        }
        if (fm->status) {
            entry.status = kb_strdup(fm->status);
        }
        mdkb_yaml_free(fm);
    }

    /* Extract content (after front-matter) */
    entry.content = mdkb_yaml_extract_content(raw_content, &entry.content_len);

    /* Get file modification time */
    struct stat st;
    if (stat(full_path, &st) == 0) {
        if (entry.timestamp == 0) {
            entry.timestamp = st.st_mtime;
        }
    }

    /* Add entry to index first to get an ID */
    mdkb_index_add_entry(index, &entry);

    /* Get the ID from the added entry */
    uint64_t entry_id = entry.id;

    /* Tokenize content and build index */
    if (entry.content) {
        size_t token_count;
        char **tokens = kb_tokenize(entry.content, &token_count);

        for (size_t i = 0; i < token_count; i++) {
            PostingList *list = ht_get(index->word_index, tokens[i]);
            if (!list) {
                list = posting_list_new();
                ht_insert(index->word_index, tokens[i], list);
            }
            posting_list_add(list, entry_id, 1);
            index->total_terms++;
        }

        kb_tokens_free(tokens, token_count);
    }

    /* Tokenize title */
    if (entry.title) {
        size_t token_count;
        char **tokens = kb_tokenize(entry.title, &token_count);

        for (size_t i = 0; i < token_count; i++) {
            PostingList *list = ht_get(index->word_index, tokens[i]);
            if (!list) {
                list = posting_list_new();
                ht_insert(index->word_index, tokens[i], list);
            }
            posting_list_add(list, entry_id, 1);
            index->total_terms++;
        }

        kb_tokens_free(tokens, token_count);
    }

    /* Cleanup (entry was copied, free original pointers) */
    free(entry.path);
    free(entry.title);
    free(entry.content);
    free(entry.raw_content);
    free(entry.status);
    if (entry.tags) {
        for (size_t i = 0; i < entry.tag_count; i++) {
            free(entry.tags[i]);
        }
        free(entry.tags);
    }

    return 0;
}

/* Recursive directory scan */
int mdkb_fs_scan_recursive(KB_Index *index, const char *dir, const char *mdkb_root) {
    DIR *dp = opendir(dir);
    if (!dp) {
        fprintf(stderr, "Warning: Cannot open directory %s: %s\n", dir, strerror(errno));
        return -1;
    }

    struct dirent *entry;
    while ((entry = readdir(dp)) != NULL) {
        /* Skip . and .. */
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        /* Build full path */
        char *full_path = kb_path_join(dir, entry->d_name);

        struct stat st;
        if (stat(full_path, &st) != 0) {
            free(full_path);
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            /* Recurse into subdirectories */
            mdkb_fs_scan_recursive(index, full_path, mdkb_root);
        } else if (S_ISREG(st.st_mode)) {
            /* Only process .md files */
            if (kb_str_ends_with(entry->d_name, ".md") ||
                kb_str_ends_with(entry->d_name, ".markdown")) {
                mdkb_fs_load_file(index, full_path, mdkb_root);
            }
        }

        free(full_path);
    }

    closedir(dp);
    return 0;
}

/* Scan knowledge base root */
int mdkb_fs_scan(KB_Index *index, const char *mdkb_root) {
    if (!index || !mdkb_root) return -1;

    /* Expand tilde */
    char *root = kb_expand_tilde(mdkb_root);
    if (!root) return -1;

    /* Check if directory exists */
    struct stat st;
    if (stat(root, &st) != 0) {
        fprintf(stderr, "Warning: KBFS root does not exist: %s\n", root);
        free(root);
        return -1;
    }

    if (!S_ISDIR(st.st_mode)) {
        fprintf(stderr, "Error: KBFS root is not a directory: %s\n", root);
        free(root);
        return -1;
    }

    /* Scan directories: try notes/ and archive/ subdirs first,
     * then fall back to scanning root directly (for knowledge/ etc.) */
    char *notes_dir = kb_path_join(root, MDKB_NOTES_DIR);
    char *archive_dir = kb_path_join(root, MDKB_ARCHIVE_DIR);

    int found_subdir = 0;
    if (stat(notes_dir, &st) == 0 && S_ISDIR(st.st_mode)) {
        mdkb_fs_scan_recursive(index, notes_dir, root);
        found_subdir = 1;
    }

    if (stat(archive_dir, &st) == 0 && S_ISDIR(st.st_mode)) {
        mdkb_fs_scan_recursive(index, archive_dir, root);
        found_subdir = 1;
    }

    /* If no notes/ or archive/ subdir, scan root directly */
    if (!found_subdir) {
        mdkb_fs_scan_recursive(index, root, root);
    }

    /* Calculate average document length */
    mdkb_index_calc_avg_length(index);

    /* Cleanup */
    free(notes_dir);
    free(archive_dir);
    free(root);

    return 0;
}
