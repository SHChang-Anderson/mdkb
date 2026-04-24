/*
 * kbfs - Knowledge Base File System
 * Main entry point
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "mdkb.h"

/* Global index (loaded once at startup) */
static KB_Index *g_index = NULL;

/* Print version */
static void print_version(void) {
    printf("%s v%s\n", MDKB_NAME, MDKB_VERSION);
    printf("High-performance knowledge base file system\n");
}

/* Print usage */
void kb_cli_usage(void) {
    printf("Usage: %s [OPTIONS] [COMMAND]\n\n", MDKB_NAME);
    printf("Commands:\n");
    printf("  (none)              Launch TUI mode\n");
    printf("  -q, --query STR     Search for keyword\n");
    printf("  --load STR          Search and output top result content\n");
    printf("  -i, --ingest PATH   Import Claude Code conversation\n");
    printf("  --pick              TUI pick mode (L key selects and exits)\n");
    printf("  --list              List recent entries as JSON (non-interactive)\n");
    printf("  --sync-on           Enable auto-sync hook\n");
    printf("  --sync-off          Disable auto-sync hook\n");
    printf("  --archive           Browse archive (raw conversations) instead of knowledge\n");
    printf("  --reindex           Rebuild search index\n");
    printf("  -v, --version       Show version\n");
    printf("  -h, --help          Show this help\n");
    printf("\nOptions:\n");
    printf("  -l, --limit N       Limit results (default: 10)\n");
    printf("  -p, --path PATH     Knowledge base root (default: ~/.mdkb)\n");
    printf("  --title-weight N    Title match weight (default: 10)\n");
    printf("  --tag-weight N      Tag match weight (default: 5)\n");
    printf("  --content-weight N  Content match weight (default: 1)\n");
    printf("\nExamples:\n");
    printf("  %s                          # Launch TUI\n", MDKB_NAME);
    printf("  %s -q \"authentication flow\"       # Search (JSON output)\n", MDKB_NAME);
    printf("  %s --load \"authentication flow\"   # Search and dump top result\n", MDKB_NAME);
    printf("  %s -q \"project setup\" -l 5            # Search, return top 5\n", MDKB_NAME);
    printf("  %s -i ~/.claude/history     # Import conversations\n", MDKB_NAME);
}

/* Parse command line options */
int kb_cli_parse(int argc, char *argv[], CLI_Options *opts) {
    /* Initialize defaults */
    memset(opts, 0, sizeof(CLI_Options));
    opts->mode = CLI_MODE_TUI;
    opts->limit = 10;
    opts->title_weight = 10;
    opts->tag_weight = 5;
    opts->content_weight = 1;
    opts->mdkb_root = NULL;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            kb_cli_usage();
            exit(0);
        } else if (strcmp(arg, "-v") == 0 || strcmp(arg, "--version") == 0) {
            print_version();
            exit(0);
        } else if (strcmp(arg, "-q") == 0 || strcmp(arg, "--query") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: --query requires an argument\n");
                return -1;
            }
            opts->mode = CLI_MODE_QUERY;
            opts->query = argv[++i];
        } else if (strcmp(arg, "--load") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: --load requires a keyword\n");
                return -1;
            }
            opts->mode = CLI_MODE_LOAD;
            opts->query = argv[++i];
        } else if (strcmp(arg, "-i") == 0 || strcmp(arg, "--ingest") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: --ingest requires a path\n");
                return -1;
            }
            opts->mode = CLI_MODE_INGEST;
            opts->ingest_path = argv[++i];
        } else if (strcmp(arg, "--pick") == 0) {
            opts->mode = CLI_MODE_PICK;
        } else if (strcmp(arg, "--list") == 0) {
            opts->mode = CLI_MODE_LIST;
        } else if (strcmp(arg, "--sync-on") == 0) {
            /* Enable sync: create toggle file + register Stop hook */
            const char *home = getenv("HOME");
            if (home) {
                char path[4096];
                snprintf(path, sizeof(path), "%s/.mdkb", home);
                mkdir(path, 0755);
                snprintf(path, sizeof(path), "%s/.mdkb/.sync-enabled", home);
                FILE *f = fopen(path, "w");
                if (f) fclose(f);
                /* Add Stop hook to Claude Code settings.json */
                (void)system("python3 /usr/local/share/mdkb/scripts/mdkb-settings.py add");
                printf("mdkb sync: ON (Stop hook registered)\n");
            }
            exit(0);
        } else if (strcmp(arg, "--sync-off") == 0) {
            /* Disable sync: remove toggle file + unregister Stop hook */
            const char *home = getenv("HOME");
            if (home) {
                char path[4096];
                snprintf(path, sizeof(path), "%s/.mdkb/.sync-enabled", home);
                unlink(path);
                /* Remove Stop hook from Claude Code settings.json */
                (void)system("python3 /usr/local/share/mdkb/scripts/mdkb-settings.py remove");
                printf("mdkb sync: OFF (Stop hook removed)\n");
            }
            exit(0);
        } else if (strcmp(arg, "--archive") == 0) {
            opts->archive_mode = 1;
        } else if (strcmp(arg, "--reindex") == 0) {
            opts->mode = CLI_MODE_INDEX;
        } else if (strcmp(arg, "-l") == 0 || strcmp(arg, "--limit") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: --limit requires a number\n");
                return -1;
            }
            opts->limit = atoi(argv[++i]);
            if (opts->limit <= 0) opts->limit = 10;
        } else if (strcmp(arg, "-p") == 0 || strcmp(arg, "--path") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: --path requires a directory\n");
                return -1;
            }
            opts->mdkb_root = argv[++i];
        } else if (strcmp(arg, "--title-weight") == 0) {
            if (i + 1 >= argc) return -1;
            opts->title_weight = atoi(argv[++i]);
        } else if (strcmp(arg, "--tag-weight") == 0) {
            if (i + 1 >= argc) return -1;
            opts->tag_weight = atoi(argv[++i]);
        } else if (strcmp(arg, "--content-weight") == 0) {
            if (i + 1 >= argc) return -1;
            opts->content_weight = atoi(argv[++i]);
        } else if (arg[0] == '-') {
            fprintf(stderr, "Error: Unknown option %s\n", arg);
            return -1;
        } else {
            /* Positional arguments not yet supported */
            fprintf(stderr, "Error: Unexpected argument %s\n", arg);
            return -1;
        }
    }

    return 0;
}

/* Query mode: output JSON for AI consumption */
int kb_cli_query(KB_Index *index, CLI_Options *opts) {
    if (!index || !opts->query) return -1;

    SearchResults *results = mdkb_search_weighted(index, opts->query,
                                                 opts->title_weight,
                                                 opts->tag_weight,
                                                 opts->content_weight);
    if (!results) {
        printf("[]\n");
        return 0;
    }

    /* Output as JSON */
    printf("[\n");
    for (size_t i = 0; i < results->count && i < (size_t)opts->limit; i++) {
        SearchResult *r = &results->results[i];
        KB_Entry *entry = mdkb_index_get_entry(index, r->entry_id);
        if (!entry) continue;

        printf("  {\n");
        printf("    \"id\": %lu,\n", (unsigned long)entry->id);
        printf("    \"path\": \"%s\",\n", entry->path);
        printf("    \"title\": \"%s\",\n", entry->title ? entry->title : "");
        printf("    \"score\": %.4f,\n", r->score);
        printf("    \"timestamp\": %ld\n", (long)entry->timestamp);
        printf("  }%s\n", (i < results->count - 1 && i < (size_t)opts->limit - 1) ? "," : "");
    }
    printf("]\n");

    mdmdkb_search_free(results);
    return 0;
}

/* Load mode: search and output the top result's full file content */
int kb_cli_load(KB_Index *index, CLI_Options *opts, const char *mdkb_root) {
    if (!index || !opts->query || !mdkb_root) return -1;

    SearchResults *results = mdkb_search_weighted(index, opts->query,
                                                 opts->title_weight,
                                                 opts->tag_weight,
                                                 opts->content_weight);
    if (!results || results->count == 0) {
        fprintf(stderr, "No results found for: %s\n", opts->query);
        if (results) mdmdkb_search_free(results);
        return 1;
    }

    /* Get the top result */
    KB_Entry *entry = mdkb_index_get_entry(index, results->results[0].entry_id);
    if (!entry || !entry->path) {
        mdmdkb_search_free(results);
        return 1;
    }

    /* Build full path and read file */
    char *full_path = kb_path_join(mdkb_root, entry->path);
    if (!full_path) {
        mdmdkb_search_free(results);
        return 1;
    }

    FILE *f = fopen(full_path, "r");
    if (!f) {
        fprintf(stderr, "Error: Cannot open %s\n", full_path);
        free(full_path);
        mdmdkb_search_free(results);
        return 1;
    }

    /* Dump file content to stdout */
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        fwrite(buf, 1, n, stdout);
    }
    fclose(f);

    /* If more results, hint about them */
    if (results->count > 1) {
        fprintf(stderr, "\n--- %zu more results available. Use -q \"%s\" to see all. ---\n",
                results->count - 1, opts->query);
    }

    free(full_path);
    mdmdkb_search_free(results);
    return 0;
}

/* List mode: output recent entries as JSON (non-interactive, for Claude Code) */
int kb_cli_list(KB_Index *index, CLI_Options *opts) {
    if (!index) return -1;

    /* Entries are already sorted by time (newest first) */
    printf("[\n");
    size_t limit = (size_t)opts->limit;
    size_t count = 0;
    for (size_t i = 0; i < index->entry_count && count < limit; i++) {
        KB_Entry *entry = &index->entries[i];
        if (!entry->path) continue;

        if (count > 0) printf(",\n");
        printf("  {\n");
        printf("    \"id\": %lu,\n", (unsigned long)entry->id);
        printf("    \"path\": \"%s\",\n", entry->path);
        printf("    \"title\": \"%s\",\n", entry->title ? entry->title : "");
        printf("    \"timestamp\": %ld\n", (long)entry->timestamp);
        printf("  }");
        count++;
    }
    printf("\n]\n");
    return 0;
}

/* Ingest mode */
int kb_cli_ingest(CLI_Options *opts) {
    if (!opts->ingest_path) return -1;

    /* Default output: ~/.mdkb/archive/claude (raw conversations) */
    const char *out = opts->mdkb_root ? opts->mdkb_root : "~/.mdkb/archive/claude";
    printf("Importing from : %s\n", opts->ingest_path);
    printf("Output dir     : %s\n", out);

    KB_Index *tmp = mdkb_index_new();
    int ret = mdkb_ingest_claude(tmp, opts->ingest_path, out);
    mdkb_index_free(tmp);
    return ret;
}

/* Main entry point */
int main(int argc, char *argv[]) {
    CLI_Options opts;

    /* Parse command line */
    if (kb_cli_parse(argc, argv, &opts) != 0) {
        return 1;
    }

    /* Handle special modes */
    if (opts.mode == CLI_MODE_VERSION) {
        print_version();
        return 0;
    }

    if (opts.mode == CLI_MODE_INGEST) {
        return kb_cli_ingest(&opts);
    }

    /* Initialize index */
    g_index = mdkb_index_new();
    if (!g_index) {
        fprintf(stderr, "Error: Failed to create index\n");
        return 1;
    }

    /* Get KBFS root */
    char *mdkb_root = opts.mdkb_root;
    if (!mdkb_root) {
        const char *home = getenv("HOME");
        if (!home) {
            fprintf(stderr, "Error: HOME not set\n");
            return 1;
        }
        /* Default to knowledge; archive is opt-in via --archive (loading 12MB of
         * conversation history takes ~3s vs knowledge's ~0.1s). */
        int use_archive = opts.archive_mode;
        mdkb_root = kb_path_join(home, use_archive ? ".mdkb/archive" : ".mdkb/knowledge");
    } else {
        mdkb_root = kb_expand_tilde(mdkb_root);
    }

    /* Scan knowledge base - use /dev/tty for status in TUI mode to keep stdout clean */
    FILE *status_out = stderr;
    if (opts.mode == CLI_MODE_TUI || opts.mode == CLI_MODE_PICK) {
        FILE *tty = fopen("/dev/tty", "w");
        if (tty) status_out = tty;
    }
    fprintf(status_out, "Loading knowledge base from: %s\n", mdkb_root);
    if (mdkb_fs_scan(g_index, mdkb_root) != 0) {
        fprintf(status_out, "Warning: Failed to scan some files\n");
    }

    /* Sort by time for archive/TUI (newest first), by path for knowledge mode */
    int use_time_sort = opts.archive_mode ||
                        opts.mode == CLI_MODE_TUI || opts.mode == CLI_MODE_PICK;
    if (use_time_sort)
        mdkb_index_sort_by_time(g_index);
    else
        mdkb_index_sort_by_path(g_index);
    fprintf(status_out, "Loaded %zu entries\n", g_index->entry_count);
    if (status_out != stderr) fclose(status_out);

    /* Execute mode */
    int result = 0;
    switch (opts.mode) {
        case CLI_MODE_QUERY:
            result = kb_cli_query(g_index, &opts);
            break;

        case CLI_MODE_LOAD:
            result = kb_cli_load(g_index, &opts, mdkb_root);
            break;

        case CLI_MODE_LIST:
            result = kb_cli_list(g_index, &opts);
            break;

        case CLI_MODE_TUI:
        case CLI_MODE_PICK:
            if (mdkb_tui_init() == 0) {
                result = mdkb_tui_run(g_index, mdkb_root);
                mdkb_tui_cleanup();
            } else {
                fprintf(stderr, "Error: Failed to initialize TUI\n");
                result = 1;
            }
            break;

        case CLI_MODE_INDEX:
            printf("Index rebuilt: %zu entries\n", g_index->entry_count);
            break;

        default:
            break;
    }

    /* Cleanup */
    mdkb_index_free(g_index);
    if (mdkb_root != opts.mdkb_root) {
        free(mdkb_root);
    }

    return result;
}
