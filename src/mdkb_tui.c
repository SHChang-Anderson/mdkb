/* mkstemp requires POSIX.1-2008 */
#define _POSIX_C_SOURCE 200809L

/*
 * kbfs - Knowledge Base File System
 * TUI (Terminal User Interface) using ncurses
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <ncurses.h>
#include <locale.h>
#include <stdbool.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>
#include <limits.h>
#include "mdkb.h"

/* inotify-based auto-refresh (Linux only) */
#ifdef __linux__
#include <sys/inotify.h>

#define INOTIFY_BUF_SIZE 4096
static int g_inotify_fd = -1;
static int *g_watch_fds = NULL;
static size_t g_watch_count = 0;
static size_t g_watch_cap = 0;

static void inotify_add_dir(const char *path) {
    if (g_inotify_fd < 0) return;
    int wd = inotify_add_watch(g_inotify_fd, path,
                               IN_CREATE | IN_MODIFY | IN_DELETE |
                               IN_MOVED_TO | IN_MOVED_FROM);
    if (wd < 0) return;
    if (g_watch_count >= g_watch_cap) {
        g_watch_cap = g_watch_cap == 0 ? 64 : g_watch_cap * 2;
        g_watch_fds = realloc(g_watch_fds, g_watch_cap * sizeof(int));
    }
    g_watch_fds[g_watch_count++] = wd;
}

static void inotify_add_recursive(const char *path) {
    inotify_add_dir(path);
    DIR *dp = opendir(path);
    if (!dp) return;
    struct dirent *ent;
    while ((ent = readdir(dp)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char full[4096];
        snprintf(full, sizeof(full), "%s/%s", path, ent->d_name);
        struct stat st;
        if (stat(full, &st) == 0 && S_ISDIR(st.st_mode))
            inotify_add_recursive(full);
    }
    closedir(dp);
}

static void inotify_remove_all_watches(void) {
    if (g_inotify_fd < 0) return;
    for (size_t i = 0; i < g_watch_count; i++)
        inotify_rm_watch(g_inotify_fd, g_watch_fds[i]);
    g_watch_count = 0;
}

static void inotify_setup(const char *root) {
    if (g_inotify_fd < 0) {
        g_inotify_fd = inotify_init1(IN_NONBLOCK);
        if (g_inotify_fd < 0) return;
    } else {
        inotify_remove_all_watches();
    }
    char *expanded = kb_expand_tilde(root);
    if (expanded) {
        inotify_add_recursive(expanded);
        free(expanded);
    }
}

static int inotify_check(void) {
    if (g_inotify_fd < 0) return 0;
    char buf[INOTIFY_BUF_SIZE]
        __attribute__((aligned(__alignof__(struct inotify_event))));
    int changed = 0;
    ssize_t len;
    while ((len = read(g_inotify_fd, buf, sizeof(buf))) > 0) {
        char *ptr = buf;
        while (ptr < buf + len) {
            struct inotify_event *ev = (struct inotify_event *)ptr;
            if (ev->len > 0) {
                size_t nlen = strlen(ev->name);
                if ((nlen > 3 && strcmp(ev->name + nlen - 3, ".md") == 0) ||
                    (ev->mask & IN_ISDIR)) {
                    changed = 1;
                }
                if ((ev->mask & IN_CREATE) && (ev->mask & IN_ISDIR))
                    changed = 1;
            }
            ptr += sizeof(struct inotify_event) + ev->len;
        }
    }
    return changed;
}

static void inotify_cleanup(void) {
    if (g_inotify_fd >= 0) {
        inotify_remove_all_watches();
        close(g_inotify_fd);
        g_inotify_fd = -1;
    }
    free(g_watch_fds);
    g_watch_fds = NULL;
    g_watch_count = 0;
    g_watch_cap = 0;
}

#elif defined(__APPLE__)
/* macOS: kqueue-based directory watching */
#include <sys/event.h>
#include <fcntl.h>

static int g_kqueue_fd = -1;
static int *g_watch_fds = NULL;
static size_t g_watch_count = 0;
static size_t g_watch_cap = 0;

static void kqueue_add_dir(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return;
    struct kevent ev;
    EV_SET(&ev, fd, EVFILT_VNODE, EV_ADD | EV_CLEAR,
           NOTE_WRITE | NOTE_DELETE | NOTE_RENAME | NOTE_EXTEND, 0, NULL);
    kevent(g_kqueue_fd, &ev, 1, NULL, 0, NULL);
    if (g_watch_count >= g_watch_cap) {
        g_watch_cap = g_watch_cap == 0 ? 64 : g_watch_cap * 2;
        g_watch_fds = realloc(g_watch_fds, g_watch_cap * sizeof(int));
    }
    g_watch_fds[g_watch_count++] = fd;
}

static void kqueue_add_recursive(const char *path) {
    kqueue_add_dir(path);
    DIR *dp = opendir(path);
    if (!dp) return;
    struct dirent *ent;
    while ((ent = readdir(dp)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char full[4096];
        snprintf(full, sizeof(full), "%s/%s", path, ent->d_name);
        struct stat st;
        if (stat(full, &st) == 0 && S_ISDIR(st.st_mode))
            kqueue_add_recursive(full);
    }
    closedir(dp);
}

static void inotify_setup(const char *root) {
    if (g_kqueue_fd < 0) {
        g_kqueue_fd = kqueue();
        if (g_kqueue_fd < 0) return;
    } else {
        for (size_t i = 0; i < g_watch_count; i++)
            close(g_watch_fds[i]);
        g_watch_count = 0;
    }
    char *expanded = kb_expand_tilde(root);
    if (expanded) {
        kqueue_add_recursive(expanded);
        free(expanded);
    }
}

static int inotify_check(void) {
    if (g_kqueue_fd < 0) return 0;
    struct kevent ev;
    struct timespec ts = {0, 0};  /* non-blocking poll */
    return kevent(g_kqueue_fd, NULL, 0, &ev, 1, &ts) > 0 ? 1 : 0;
}

static void inotify_cleanup(void) {
    for (size_t i = 0; i < g_watch_count; i++)
        close(g_watch_fds[i]);
    free(g_watch_fds);
    g_watch_fds = NULL;
    g_watch_count = 0;
    g_watch_cap = 0;
    if (g_kqueue_fd >= 0) {
        close(g_kqueue_fd);
        g_kqueue_fd = -1;
    }
}

#else /* other platforms: no-op stubs */

static void inotify_setup(const char *root)  { (void)root; }
static int  inotify_check(void)              { return 0; }
static void inotify_cleanup(void)            {}

#endif /* __linux__ / __APPLE__ */

/* Unicode line-drawing helpers — more reliable than ACS on macOS */
static void ui_hline(int y, int x, int n) {
    move(y, x);
    for (int i = 0; i < n; i++) addstr("─");
}
static void ui_vline(int y, int x, int n) {
    for (int i = 0; i < n; i++) mvaddstr(y + i, x, "│");
}

/* CTRL macro */
#ifndef CTRL
#define CTRL(c) ((c) & 0x1f)
#endif

/* Color pairs */
#define CP_DEFAULT    1
#define CP_HIGHLIGHT  2
#define CP_HEADER     3
#define CP_CODE       4
#define CP_TAG        5
#define CP_LINK       6
#define CP_QUOTE      7
#define CP_LIST       8
#define CP_EMPHASIS   9
#define CP_SELECTION  10
#define CP_CURSOR     11
#define CP_HUMAN      12
#define CP_CLAUDE     13
#define CP_CODE_BORDER 14
#define CP_POPUP       15  /* popup body: bright white on black */
#define CP_POPUP_SEL   16  /* popup selected row */

/* Modes */
#define MODE_LIST    0
#define MODE_READER  1

/* Reader state */
typedef struct {
    KB_Entry *entry;
    char **lines;
    size_t line_count;
    size_t line_capacity;
    int cursor_y;
    int cursor_x;
    int scroll_y;
    int scroll_x;
    int visual_mode;
    int visual_start_y;
    int visual_start_x;
    char *search_pattern;
    int search_direction;  /* 1 = forward, -1 = backward */
    int in_code_block;
    int preview_mode;      /* 1 = preview (rendered), 0 = edit mode (with cursor) */
    int raw_mode;          /* 1 = raw text display, 0 = rendered */
    int ai_only_mode;      /* 1 = filter out Human sections, show only Claude responses */
} ReaderState;

/* TUI state */
typedef struct {
    KB_Index *index;
    char *mdkb_root;
    SearchResults *results;
    int selected;
    int offset;
    int top_offset;
    char *search_query;
    int mode;
    int left_width;
    int running;
    int in_code_block;
    int preview_match_line;  /* current n/N match line in right pane (-1 = none) */
    char *picked_path;       /* set by 'L' key: full path of selected entry for --pick mode */
    int archive_mode;        /* 1 = archive (conversations), 0 = knowledge (extracted) */
    char *alt_root;          /* root path for the other mode (for Tab switching) */
    bool *marked;            /* marked[i] = true if entry i is marked for multi-select */
    size_t mark_count;       /* number of marked entries */
    bool *loaded_marks;      /* snapshot of marks at last launch (tracks loaded files) */
    size_t loaded_mark_count; /* number of loaded marks */
    char **saved_mark_paths;         /* saved marked paths from previous Tab mode */
    size_t saved_mark_path_count;
    char **saved_loaded_mark_paths;  /* saved loaded_marks paths from previous Tab mode */
    size_t saved_loaded_mark_path_count;
    char *topic_filter;              /* active topic filter string, NULL = show all */
    char *type_filter;               /* active type filter: "knowledge"/"code"/"workflow", NULL = show all */
    char **tag_filters;              /* active tag filter strings, NULL = show all */
    size_t tag_filter_count;         /* number of active tag filters */
    /* Dual-index cache: keep both archive and knowledge indexes in memory */
    KB_Index *archive_index;         /* cached archive index (NULL = not yet loaded) */
    KB_Index *knowledge_index;       /* cached knowledge index (NULL = not yet loaded) */
    char *archive_root;              /* root path for archive mode */
    char *knowledge_root;            /* root path for knowledge mode */
    bool *archive_marked;            /* marks for archive mode */
    size_t archive_mark_count;
    bool *archive_loaded_marks;
    size_t archive_loaded_mark_count;
    bool *knowledge_marked;          /* marks for knowledge mode */
    size_t knowledge_mark_count;
    bool *knowledge_loaded_marks;
    size_t knowledge_loaded_mark_count;
    /* Filtered entry cache: pre-computed index list for O(1) lookup */
    int *filtered_indices;           /* array of raw indices matching current filter */
    size_t filtered_count;           /* number of entries in filtered_indices */
    bool filter_cache_valid;         /* true if filtered_indices is up to date */
    /* Match line cache: avoid re-scanning content on every j/k */
    uint64_t cached_match_entry_id;  /* entry ID for which match cache is valid */
    char *cached_match_query;        /* search query for which match cache is valid */
    int *cached_match_lines;         /* array of matching line numbers */
    int cached_match_count;          /* number of cached match lines */
    ReaderState reader;
} TUI_State;

static TUI_State g_tui = {0};

/* Claude Code child process tracking */
static pid_t g_claude_pid = 0;
static char g_claude_session_id[64] = "";

/* /dev/tty handles for running inside piped environments (e.g. Claude Code) */
static FILE *g_tty_in = NULL;
static FILE *g_tty_out = NULL;
static SCREEN *g_screen = NULL;

/* Forward declarations */
static void reader_init(KB_Entry *entry);
static void reader_cleanup(void);
static void reader_run(void);
static void yank_to_clipboard(const char *tmp_path);
static KB_Index *load_index_for_root(const char *root);
static void tui_switch_to_index(KB_Index *new_index, const char *new_root);
static void invalidate_filter_cache(void);
static size_t effective_count(void);
static KB_Entry *effective_entry_at(int idx);

/* Merge-scan: add new files to existing index without clearing old entries.
 * Preserves current selection position. */
static void tui_merge_new_entries(void) {
    if (!g_tui.mdkb_root || !g_tui.index) return;

    /* Remember current selection path to restore after sort */
    char *sel_path = NULL;
    if (g_tui.selected >= 0 && (size_t)g_tui.selected < g_tui.index->entry_count) {
        KB_Entry *cur = &g_tui.index->entries[g_tui.selected];
        if (cur->path) sel_path = kb_strdup(cur->path);
    }

    size_t old_count = g_tui.index->entry_count;
    size_t old_capacity = g_tui.index->entry_capacity;

    /* Scan only the current mode's directory — don't mix archive/knowledge */
    mdkb_fs_scan(g_tui.index, g_tui.mdkb_root);

    /* Grow marked/loaded_marks arrays if capacity increased */
    if (g_tui.index->entry_capacity > old_capacity) {
        size_t new_cap = g_tui.index->entry_capacity;
        g_tui.marked = realloc(g_tui.marked, new_cap * sizeof(bool));
        memset(g_tui.marked + old_capacity, 0, (new_cap - old_capacity) * sizeof(bool));
        g_tui.loaded_marks = realloc(g_tui.loaded_marks, new_cap * sizeof(bool));
        memset(g_tui.loaded_marks + old_capacity, 0, (new_cap - old_capacity) * sizeof(bool));
    }

    if (g_tui.index->entry_count > old_count) {
        /* marked[] is indexed by entry ID, not array position,
         * so sort does not invalidate marks. Just re-sort. */
        mdkb_index_sort_by_time(g_tui.index);

        /* Try to restore selection to the same entry */
        if (sel_path) {
            for (size_t i = 0; i < g_tui.index->entry_count; i++) {
                if (g_tui.index->entries[i].path &&
                    strcmp(g_tui.index->entries[i].path, sel_path) == 0) {
                    g_tui.selected = (int)i;
                    /* Adjust scroll so selection is visible */
                    int max_y, max_x;
                    getmaxyx(stdscr, max_y, max_x);
                    (void)max_x;
                    int visible = max_y - 3;
                    if (g_tui.selected < g_tui.offset)
                        g_tui.offset = g_tui.selected;
                    else if (g_tui.selected >= g_tui.offset + visible)
                        g_tui.offset = g_tui.selected - visible + 1;
                    break;
                }
            }
        }
    }

    free(sel_path);
}

/* Full rescan of the current mode's index (used after Claude Code exits) */
static void tui_reload_current_index(void) {
    if (!g_tui.mdkb_root) return;

    /* Free old search and filter state */
    if (g_tui.search_query) { free(g_tui.search_query); g_tui.search_query = NULL; }
    if (g_tui.results) { mdmdkb_search_free(g_tui.results); g_tui.results = NULL; }
    if (g_tui.topic_filter) { free(g_tui.topic_filter); g_tui.topic_filter = NULL; }
    if (g_tui.type_filter) { free(g_tui.type_filter); g_tui.type_filter = NULL; }
    for (size_t i = 0; i < g_tui.tag_filter_count; i++) free(g_tui.tag_filters[i]);
    free(g_tui.tag_filters); g_tui.tag_filters = NULL; g_tui.tag_filter_count = 0;

    /* Rescan: clear entries and re-read from disk */
    for (size_t i = 0; i < g_tui.index->entry_count; i++)
        mdkb_entry_free(&g_tui.index->entries[i]);
    g_tui.index->entry_count = 0;
    mdkb_fs_scan(g_tui.index, g_tui.mdkb_root);
    mdkb_index_sort_by_time(g_tui.index);

    /* Reset selection */
    g_tui.selected = 0;
    g_tui.offset = 0;
    g_tui.top_offset = 0;

    /* Reallocate marks for new capacity */
    free(g_tui.marked);
    g_tui.marked = calloc(g_tui.index->entry_capacity, sizeof(bool));
    g_tui.mark_count = 0;
    free(g_tui.loaded_marks);
    g_tui.loaded_marks = calloc(g_tui.index->entry_capacity, sizeof(bool));
    g_tui.loaded_mark_count = 0;

    /* Update the cached slot too */
    if (g_tui.archive_mode) {
        g_tui.archive_marked = g_tui.marked;
        g_tui.archive_loaded_marks = g_tui.loaded_marks;
    } else {
        g_tui.knowledge_marked = g_tui.marked;
        g_tui.knowledge_loaded_marks = g_tui.loaded_marks;
    }

    invalidate_filter_cache();
}

/* Restore TUI after returning from Claude Code.
 * full_reload: 1 = destroy + rescan (Claude exited), 0 = merge new entries (Ctrl+Z suspend) */
static void restore_after_claude(int full_reload) {
    /* Drain any stale terminal responses (e.g. device attribute queries)
     * left in the input buffer by claude before ncurses reads them. */
    tcflush(STDIN_FILENO, TCIFLUSH);
    mdkb_tui_init();
    if (!g_tui.mdkb_root) return;
    if (full_reload)
        tui_reload_current_index();
    else
        tui_merge_new_entries();
}

/* Initialize colors */
static void init_colors(void) {
    if (has_colors()) {
        start_color();
        use_default_colors();

        init_pair(CP_DEFAULT, -1, -1);
        init_pair(CP_HIGHLIGHT, COLOR_BLACK, COLOR_WHITE);
        init_pair(CP_HEADER, COLOR_BLUE, -1);
        init_pair(CP_CODE, COLOR_GREEN, -1);
        init_pair(CP_TAG, COLOR_YELLOW, -1);
        init_pair(CP_LINK, COLOR_CYAN, -1);
        init_pair(CP_QUOTE, COLOR_MAGENTA, -1);
        init_pair(CP_LIST, COLOR_YELLOW, -1);
        init_pair(CP_EMPHASIS, COLOR_WHITE, -1);
        init_pair(CP_SELECTION, COLOR_BLACK, COLOR_WHITE);
        init_pair(CP_CURSOR, COLOR_WHITE, COLOR_BLUE);
        init_pair(CP_HUMAN, COLOR_WHITE, COLOR_BLUE);
        init_pair(CP_CLAUDE, COLOR_WHITE, COLOR_MAGENTA);
        init_pair(CP_CODE_BORDER, COLOR_CYAN, -1);
        init_pair(CP_POPUP,     COLOR_WHITE, COLOR_BLACK);
        init_pair(CP_POPUP_SEL, COLOR_BLACK, COLOR_WHITE);
    }
}

/* Draw header */
static void draw_header(void) {
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);
    (void)max_y;

    attron(A_BOLD | COLOR_PAIR(CP_HEADER));
    mvprintw(0, 0, " %s v%s", MDKB_NAME, MDKB_VERSION);

    /* Show current mode */
    if (g_tui.archive_mode) {
        attron(COLOR_PAIR(CP_LINK));
        printw(" [archive]");
        attroff(COLOR_PAIR(CP_LINK));
        attron(A_BOLD | COLOR_PAIR(CP_HEADER));
    } else {
        attron(COLOR_PAIR(CP_CODE));
        printw(" [knowledge]");
        attroff(COLOR_PAIR(CP_CODE));
        attron(A_BOLD | COLOR_PAIR(CP_HEADER));
    }

    if (g_tui.topic_filter) {
        attroff(A_BOLD | COLOR_PAIR(CP_HEADER));
        attron(COLOR_PAIR(CP_QUOTE));
        printw(" | Topic: %s", g_tui.topic_filter);
        attroff(COLOR_PAIR(CP_QUOTE));
        attron(A_BOLD | COLOR_PAIR(CP_HEADER));
    }

    if (g_tui.type_filter) {
        attroff(A_BOLD | COLOR_PAIR(CP_HEADER));
        attron(COLOR_PAIR(CP_QUOTE));
        printw(" | Type: %s", g_tui.type_filter);
        attroff(COLOR_PAIR(CP_QUOTE));
        attron(A_BOLD | COLOR_PAIR(CP_HEADER));
    }

    if (g_tui.tag_filter_count > 0) {
        attroff(A_BOLD | COLOR_PAIR(CP_HEADER));
        attron(COLOR_PAIR(CP_QUOTE));
        printw(" | Tag:");
        for (size_t ti = 0; ti < g_tui.tag_filter_count; ti++)
            printw(" %s", g_tui.tag_filters[ti]);
        attroff(COLOR_PAIR(CP_QUOTE));
        attron(A_BOLD | COLOR_PAIR(CP_HEADER));
    }

    if (g_tui.search_query) {
        printw(" | Search: %s", g_tui.search_query);
    }

    printw(" | %zu entries", effective_count());

    /* Show draft count if any */
    if (!g_tui.archive_mode) {
        size_t draft_n = 0;
        for (size_t di = 0; di < g_tui.index->entry_count; di++) {
            KB_Entry *de = &g_tui.index->entries[di];
            if (de->status && strcmp(de->status, "draft") == 0) draft_n++;
        }
        if (draft_n > 0) {
            attroff(A_BOLD | COLOR_PAIR(CP_HEADER));
            attron(COLOR_PAIR(CP_QUOTE) | A_BOLD);
            printw(" | Inbox: %zu", draft_n);
            attroff(COLOR_PAIR(CP_QUOTE) | A_BOLD);
            attron(A_BOLD | COLOR_PAIR(CP_HEADER));
        }
    }

    if (g_claude_pid > 0) {
        attron(COLOR_PAIR(CP_TAG));
        if (g_claude_session_id[0])
            printw(" | [Claude %.8s.. - R]", g_claude_session_id);
        else
            printw(" | [Claude paused - R]");
        attroff(COLOR_PAIR(CP_TAG));
        attron(A_BOLD | COLOR_PAIR(CP_HEADER));
    }

    /* Right-aligned help */
    const char *help = "q:quit /:search t:topic m/A:mark L:claude ?:help";
    int help_len = strlen(help);
    if (max_x > help_len + 40) {
        mvprintw(0, max_x - help_len - 1, "%s", help);
    }

    attroff(A_BOLD | COLOR_PAIR(CP_HEADER));

    /* Separator line */
    ui_hline(1, 0, max_x);
}

/* Draw a title string with case-insensitive substring highlighting.
 * Matches are shown in CP_TAG+bold; non-matches in current attr.
 * Returns the column after the last printed character. */
static int draw_title_highlighted(const char *title, const char *query,
                                  int y, int x, int max_len, int is_selected) {
    int col = x;
    int remaining = max_len;
    const char *p = title;

    /* Build lowercase copies for case-insensitive comparison */
    char lo_title[256], lo_query[256];
    strncpy(lo_title, title, sizeof(lo_title) - 1);
    lo_title[sizeof(lo_title) - 1] = '\0';
    for (char *c = lo_title; *c; c++) *c = (char)tolower((unsigned char)*c);

    if (query) {
        strncpy(lo_query, query, sizeof(lo_query) - 1);
        lo_query[sizeof(lo_query) - 1] = '\0';
        for (char *c = lo_query; *c; c++) *c = (char)tolower((unsigned char)*c);
    } else {
        lo_query[0] = '\0';
    }

    size_t qlen = strlen(lo_query);

    move(y, col);

    while (*p && remaining > 0) {
        /* Try to find query match at current position */
        if (qlen > 0) {
            const char *lo_p = lo_title + (p - title);
            if (strncmp(lo_p, lo_query, qlen) == 0 && (int)qlen <= remaining) {
                /* Print match highlighted */
                if (is_selected) attroff(COLOR_PAIR(CP_HIGHLIGHT));
                attron(COLOR_PAIR(CP_TAG) | A_BOLD);
                for (size_t k = 0; k < qlen && remaining > 0; k++, col++, remaining--) {
                    addch((unsigned char)p[k]);
                }
                attroff(COLOR_PAIR(CP_TAG) | A_BOLD);
                if (is_selected) attron(COLOR_PAIR(CP_HIGHLIGHT));
                p += qlen;
                continue;
            }
        }
        addch((unsigned char)*p++);
        col++;
        remaining--;
    }

    return col;
}

/* Draw left pane (entry list) */
/* Extract repo name from entry path (e.g. "myproject/auth/file.md" → "myproject") */
static void get_repo_from_path(const char *path, char *repo, size_t repo_size) {
    repo[0] = '\0';
    if (!path) return;
    const char *slash = strchr(path, '/');
    if (slash) {
        size_t len = (size_t)(slash - path);
        if (len >= repo_size) len = repo_size - 1;
        strncpy(repo, path, len);
        repo[len] = '\0';
    }
}

/* Extract topic from entry path (e.g. "myproject/auth/file.md" → "auth") */
static void get_topic_from_path(const char *path, char *topic, size_t topic_size) {
    topic[0] = '\0';
    if (!path) return;
    const char *first = strchr(path, '/');
    if (!first) return;
    first++;
    const char *second = strchr(first, '/');
    if (second) {
        size_t len = (size_t)(second - first);
        if (len >= topic_size) len = topic_size - 1;
        strncpy(topic, first, len);
        topic[len] = '\0';
    }
}

/* Return entry at raw (unfiltered) index, from results or entries[] */
static KB_Entry *raw_entry_at(int idx) {
    if (g_tui.results) {
        size_t cnt = g_tui.results->count;
        if (idx < 0 || (size_t)idx >= cnt) return NULL;
        return mdkb_index_get_entry(g_tui.index,
                                     g_tui.results->results[idx].entry_id);
    }
    if (idx < 0 || (size_t)idx >= g_tui.index->entry_count) return NULL;
    return &g_tui.index->entries[idx];
}

/* Check if an entry matches the active topic filter */
static int entry_matches_topic(KB_Entry *e) {
    if (!g_tui.topic_filter) return 1;
    if (!e || !e->path) return 0;
    char topic[128];
    get_topic_from_path(e->path, topic, sizeof(topic));
    return strcmp(topic, g_tui.topic_filter) == 0;
}

/* Check if an entry has a type:xxx tag matching the active type filter */
static int entry_matches_type(KB_Entry *e) {
    if (!g_tui.type_filter) return 1;
    if (!e || !e->tags) return 0;
    char needle[80];
    snprintf(needle, sizeof(needle), "type:%s", g_tui.type_filter);
    for (size_t i = 0; i < e->tag_count; i++) {
        if (strcmp(e->tags[i], needle) == 0) return 1;
    }
    return 0;
}

/* Check if an entry has a tag matching the active tag filter */
static int entry_matches_tag(KB_Entry *e) {
    if (!g_tui.tag_filters || g_tui.tag_filter_count == 0) return 1;
    if (!e || !e->tags) return 0;
    /* Entry must have at least one of the filtered tags */
    for (size_t i = 0; i < e->tag_count; i++) {
        if (strncmp(e->tags[i], "type:", 5) == 0) continue;
        for (size_t f = 0; f < g_tui.tag_filter_count; f++) {
            if (strcmp(e->tags[i], g_tui.tag_filters[f]) == 0) return 1;
        }
    }
    return 0;
}

/* Check if entry passes all active filters */
static int entry_matches_filters(KB_Entry *e) {
    return entry_matches_topic(e) && entry_matches_type(e) && entry_matches_tag(e);
}

/* Rebuild the filtered entry index cache */
static void rebuild_filter_cache(void) {
    size_t base = g_tui.results ? g_tui.results->count : g_tui.index->entry_count;
    /* No filter active: no cache needed */
    if (!g_tui.topic_filter && !g_tui.type_filter && g_tui.tag_filter_count == 0) {
        free(g_tui.filtered_indices);
        g_tui.filtered_indices = NULL;
        g_tui.filtered_count = base;
        g_tui.filter_cache_valid = true;
        return;
    }
    /* Rebuild filtered list */
    free(g_tui.filtered_indices);
    g_tui.filtered_indices = kb_malloc(base * sizeof(int));
    g_tui.filtered_count = 0;
    for (size_t i = 0; i < base; i++) {
        KB_Entry *e = raw_entry_at((int)i);
        if (entry_matches_filters(e)) {
            g_tui.filtered_indices[g_tui.filtered_count++] = (int)i;
        }
    }
    g_tui.filter_cache_valid = true;
}

/* Invalidate filter cache (call when filters, search results, or index change) */
static void invalidate_filter_cache(void) {
    g_tui.filter_cache_valid = false;
}

/* Ensure filter cache is valid */
static void ensure_filter_cache(void) {
    if (!g_tui.filter_cache_valid) rebuild_filter_cache();
}

/* Effective entry count respecting topic + type filters + search results */
static size_t effective_count(void) {
    ensure_filter_cache();
    return g_tui.filtered_count;
}

/* Map logical filtered index to KB_Entry* */
static KB_Entry *effective_entry_at(int idx) {
    ensure_filter_cache();
    if (idx < 0 || (size_t)idx >= g_tui.filtered_count) return NULL;
    if (!g_tui.filtered_indices) return raw_entry_at(idx);
    return raw_entry_at(g_tui.filtered_indices[idx]);
}

/* Set or clear topic filter */
static void set_topic_filter(const char *topic) {
    free(g_tui.topic_filter);
    g_tui.topic_filter = topic ? kb_strdup(topic) : NULL;
    g_tui.selected = 0;
    g_tui.offset = 0;
    g_tui.top_offset = 0;
    g_tui.preview_match_line = -1;
    invalidate_filter_cache();
}

/* Set or clear type filter */
static void set_type_filter(const char *type) {
    free(g_tui.type_filter);
    g_tui.type_filter = type ? kb_strdup(type) : NULL;
    g_tui.selected = 0;
    g_tui.offset = 0;
    g_tui.top_offset = 0;
    g_tui.preview_match_line = -1;
    invalidate_filter_cache();
}

/* Set or clear tag filter */
/* Clear all tag filters */
static void clear_tag_filters(void) {
    for (size_t i = 0; i < g_tui.tag_filter_count; i++)
        free(g_tui.tag_filters[i]);
    free(g_tui.tag_filters);
    g_tui.tag_filters = NULL;
    g_tui.tag_filter_count = 0;
    g_tui.selected = 0;
    g_tui.offset = 0;
    g_tui.top_offset = 0;
    g_tui.preview_match_line = -1;
    invalidate_filter_cache();
}

/* Set tag filters from an array of selected tags.
 * Takes ownership: caller must NOT free the array or strings. */
static void set_tag_filters(char **tags, size_t count) {
    for (size_t i = 0; i < g_tui.tag_filter_count; i++)
        free(g_tui.tag_filters[i]);
    free(g_tui.tag_filters);
    g_tui.tag_filters = tags;
    g_tui.tag_filter_count = count;
    g_tui.selected = 0;
    g_tui.offset = 0;
    g_tui.top_offset = 0;
    g_tui.preview_match_line = -1;
    invalidate_filter_cache();
}

/* Count entries matching a specific type tag */
static size_t count_entries_for_type(const char *type) {
    char needle[80];
    snprintf(needle, sizeof(needle), "type:%s", type);
    size_t n = 0;
    for (size_t i = 0; i < g_tui.index->entry_count; i++) {
        KB_Entry *e = &g_tui.index->entries[i];
        /* Only count entries matching current topic/tag filters */
        if (!entry_matches_topic(e) || !entry_matches_tag(e)) continue;
        if (!e->tags) continue;
        for (size_t j = 0; j < e->tag_count; j++) {
            if (strcmp(e->tags[j], needle) == 0) { n++; break; }
        }
    }
    return n;
}

/* Draw type filter popup overlay. Returns selected type (strdup'd) or NULL.
 * Only shows types that have entries matching current topic/tag filters. */
static char *do_type_filter_popup(void) {
    static const char *all_types[] = {"knowledge", "code", "workflow"};
    static const int all_type_count = 3;

    /* Collect only types with count > 0 under current filters */
    const char *visible_types[3];
    size_t visible_counts[3];
    int visible_count = 0;
    for (int i = 0; i < all_type_count; i++) {
        size_t cnt = count_entries_for_type(all_types[i]);
        if (cnt > 0) {
            visible_types[visible_count] = all_types[i];
            visible_counts[visible_count] = cnt;
            visible_count++;
        }
    }
    if (visible_count == 0) return NULL;

    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);

    int popup_h = visible_count + 4;  /* border + title + types + border */
    int popup_w = 32;
    int start_y = (max_y - popup_h) / 2;
    int start_x = (max_x - popup_w) / 2;

    int sel = 0;
    int ch;

    while (1) {
        /* Fill entire popup with CP_POPUP background — keep it active throughout
         * so every cell has an opaque background. Switch temporarily for accents. */
        attron(COLOR_PAIR(CP_POPUP));
        for (int row = 0; row < popup_h; row++)
            mvhline(start_y + row, start_x, ' ', popup_w);

        /* Border (temporarily switch to CP_HEADER, then restore) */
        attron(A_BOLD | COLOR_PAIR(CP_HEADER));
        ui_hline(start_y, start_x, popup_w);
        ui_hline(start_y + popup_h - 1, start_x, popup_w);
        for (int row = 0; row < popup_h; row++) {
            mvaddstr(start_y + row, start_x, "│");
            mvaddstr(start_y + row, start_x + popup_w - 1, "│");
        }
        mvaddstr(start_y, start_x, "┌");
        mvaddstr(start_y, start_x + popup_w - 1, "┐");
        mvaddstr(start_y + popup_h - 1, start_x, "└");
        mvaddstr(start_y + popup_h - 1, start_x + popup_w - 1, "┘");
        mvprintw(start_y + 1, start_x + 2, "Select Type:");
        attroff(A_BOLD | COLOR_PAIR(CP_HEADER));
        attron(COLOR_PAIR(CP_POPUP));

        /* Separator */
        attron(COLOR_PAIR(CP_HEADER));
        ui_hline(start_y + 2, start_x + 1, popup_w - 2);
        attroff(COLOR_PAIR(CP_HEADER));
        attron(COLOR_PAIR(CP_POPUP));

        /* Items */
        for (int ti = 0; ti < visible_count; ti++) {
            int row = start_y + 3 + ti;
            if (ti == sel) { attroff(COLOR_PAIR(CP_POPUP)); attron(COLOR_PAIR(CP_POPUP_SEL)); }
            mvprintw(row, start_x + 1, " %c %-12s (%zu)%-*s",
                     ti == sel ? '>' : ' ', visible_types[ti],
                     visible_counts[ti], (int)(popup_w - 22), "");
            if (ti == sel) { attroff(COLOR_PAIR(CP_POPUP_SEL)); attron(COLOR_PAIR(CP_POPUP)); }
        }
        attroff(COLOR_PAIR(CP_POPUP));
        refresh();

        ch = getch();
        switch (ch) {
        case 'j': case KEY_DOWN:
            sel = (sel + 1) % visible_count;
            break;
        case 'k': case KEY_UP:
            sel = (sel - 1 + visible_count) % visible_count;
            break;
        case '\n': case '\r': {
            char *result = kb_strdup(visible_types[sel]);
            return result;
        }
        case 27: case 'q': case 'T':
            return NULL;
        }
    }
}

/* Comparison for qsort of strings */
static int cmp_search_result_by_time_desc(const void *a, const void *b) {
    const SearchResult *ra = (const SearchResult *)a;
    const SearchResult *rb = (const SearchResult *)b;
    KB_Entry *ea = mdkb_index_get_entry(g_tui.index, ra->entry_id);
    KB_Entry *eb = mdkb_index_get_entry(g_tui.index, rb->entry_id);
    time_t ta = ea ? ea->timestamp : 0;
    time_t tb = eb ? eb->timestamp : 0;
    if (tb > ta) return 1;
    if (tb < ta) return -1;
    return 0;
}

static int cmp_strings(const void *a, const void *b) {
    return strcmp(*(const char **)a, *(const char **)b);
}

/* Collect unique topics from current index, sorted alphabetically.
 * Caller must free each string and the array. */
static char **collect_unique_topics(size_t *out_count) {
    *out_count = 0;
    size_t cap = 32;
    char **topics = malloc(cap * sizeof(char *));
    if (!topics) return NULL;

    for (size_t i = 0; i < g_tui.index->entry_count; i++) {
        KB_Entry *e = &g_tui.index->entries[i];
        if (!e->path) continue;
        /* Only collect topics from entries matching current type/tag filters */
        if (!entry_matches_type(e) || !entry_matches_tag(e)) continue;
        char topic[128];
        get_topic_from_path(e->path, topic, sizeof(topic));
        if (!topic[0]) continue;

        /* Check if already collected */
        int found = 0;
        for (size_t j = 0; j < *out_count; j++) {
            if (strcmp(topics[j], topic) == 0) { found = 1; break; }
        }
        if (found) continue;

        if (*out_count >= cap) {
            cap *= 2;
            topics = realloc(topics, cap * sizeof(char *));
        }
        topics[*out_count] = kb_strdup(topic);
        (*out_count)++;
    }

    qsort(topics, *out_count, sizeof(char *), cmp_strings);
    return topics;
}

/* Collect unique tags from current index, sorted alphabetically.
 * Excludes "type:*" tags (managed by type filter).
 * Caller must free each string and the array. */
static char **collect_unique_tags(size_t *out_count) {
    *out_count = 0;
    size_t cap = 64;
    char **tags = malloc(cap * sizeof(char *));
    if (!tags) return NULL;

    for (size_t i = 0; i < g_tui.index->entry_count; i++) {
        KB_Entry *e = &g_tui.index->entries[i];
        /* Only collect tags from entries matching current topic/type filters */
        if (!entry_matches_topic(e) || !entry_matches_type(e)) continue;
        if (!e->tags) continue;
        for (size_t j = 0; j < e->tag_count; j++) {
            const char *tag = e->tags[j];
            if (!tag || strncmp(tag, "type:", 5) == 0) continue;

            /* Check if already collected */
            int found = 0;
            for (size_t k = 0; k < *out_count; k++) {
                if (strcmp(tags[k], tag) == 0) { found = 1; break; }
            }
            if (found) continue;

            if (*out_count >= cap) {
                cap *= 2;
                tags = realloc(tags, cap * sizeof(char *));
            }
            tags[*out_count] = kb_strdup(tag);
            (*out_count)++;
        }
    }

    qsort(tags, *out_count, sizeof(char *), cmp_strings);
    return tags;
}

/* Count entries matching a specific topic */
static size_t count_entries_for_topic(const char *topic) {
    size_t n = 0;
    for (size_t i = 0; i < g_tui.index->entry_count; i++) {
        KB_Entry *e = &g_tui.index->entries[i];
        if (!e->path) continue;
        /* Only count entries matching current type/tag filters */
        if (!entry_matches_type(e) || !entry_matches_tag(e)) continue;
        char t[128];
        get_topic_from_path(e->path, t, sizeof(t));
        if (strcmp(t, topic) == 0) n++;
    }
    return n;
}

/* Draw topic filter popup overlay with search. Returns selected topic (strdup'd) or NULL.
 * Includes a "New..." entry that allows creating a new topic via text input. */
static char *do_topic_filter_popup(void) {
    size_t topic_count = 0;
    char **topics = collect_unique_topics(&topic_count);

    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);

    int popup_w = 40;
    for (size_t i = 0; i < topic_count; i++) {
        int len = (int)strlen(topics[i]) + 10;
        if (len + 4 > popup_w) popup_w = len + 4;
    }
    if (popup_w > max_x - 4) popup_w = max_x - 4;

    /* Search filter state */
    char filter[128] = "";
    int filter_len = 0;

    /* Filtered indices into topics[] — index -1 means the "New..." entry */
    size_t alloc_count = topic_count + 1;
    int *filtered = kb_malloc(alloc_count * sizeof(int));
    size_t fcount = topic_count + 1;
    filtered[0] = -1;  /* "New..." entry always first */
    for (size_t i = 0; i < topic_count; i++) filtered[i + 1] = (int)i;

    int sel = 0;
    int scroll = 0;
    char *result = NULL;

    while (1) {
        /* Recalculate popup height based on filtered count + search bar */
        int popup_h = (int)fcount + 6;  /* border + title + separator + search + items + border */
        if (popup_h > max_y - 4) popup_h = max_y - 4;
        if (popup_h < 7) popup_h = 7;
        int visible = popup_h - 6;  /* rows available for topic items */

        int start_y = (max_y - popup_h) / 2;
        int start_x = (max_x - popup_w) / 2;

        /* Fill entire popup with CP_POPUP background — keep active throughout
         * so every cell has an opaque background. Switch temporarily for accents. */
        attron(COLOR_PAIR(CP_POPUP));
        for (int y = start_y; y < start_y + popup_h; y++)
            mvhline(y, start_x, ' ', popup_w);

        /* Border (temporarily CP_HEADER, then restore CP_POPUP) */
        attron(A_BOLD | COLOR_PAIR(CP_HEADER));
        ui_hline(start_y, start_x, popup_w);
        ui_hline(start_y + popup_h - 1, start_x, popup_w);
        for (int y = start_y; y < start_y + popup_h; y++) {
            mvaddstr(y, start_x, "│");
            mvaddstr(y, start_x + popup_w - 1, "│");
        }
        mvaddstr(start_y, start_x, "┌");
        mvaddstr(start_y, start_x + popup_w - 1, "┐");
        mvaddstr(start_y + popup_h - 1, start_x, "└");
        mvaddstr(start_y + popup_h - 1, start_x + popup_w - 1, "┘");
        mvprintw(start_y + 1, start_x + 2, "Select Topic:");
        attroff(A_BOLD | COLOR_PAIR(CP_HEADER));
        attron(COLOR_PAIR(CP_POPUP));

        /* Separator below title */
        attron(COLOR_PAIR(CP_HEADER));
        ui_hline(start_y + 2, start_x + 1, popup_w - 2);
        attroff(COLOR_PAIR(CP_HEADER));
        attron(COLOR_PAIR(CP_POPUP));

        /* Search bar */
        mvhline(start_y + 3, start_x + 1, ' ', popup_w - 2);
        attron(COLOR_PAIR(CP_TAG));
        mvprintw(start_y + 3, start_x + 2, "/ %s", filter);
        attroff(COLOR_PAIR(CP_TAG));
        attron(COLOR_PAIR(CP_POPUP));

        /* Separator below search */
        attron(COLOR_PAIR(CP_HEADER));
        ui_hline(start_y + 4, start_x + 1, popup_w - 2);
        attroff(COLOR_PAIR(CP_HEADER));
        attron(COLOR_PAIR(CP_POPUP));

        /* Draw filtered topic list */
        if (fcount == 0) {
            attron(A_DIM);
            mvprintw(start_y + 5, start_x + 4, "(no match)");
            attroff(A_DIM);
        } else {
            for (int i = 0; i < visible && (size_t)(scroll + i) < fcount; i++) {
                int fidx = scroll + i;
                int tidx = filtered[fidx];
                int y = start_y + 5 + i;

                if (fidx == sel) { attroff(COLOR_PAIR(CP_POPUP)); attron(COLOR_PAIR(CP_POPUP_SEL)); }
                mvhline(y, start_x + 1, ' ', popup_w - 2);

                if (tidx == -1) {
                    attron(A_BOLD);
                    mvprintw(y, start_x + 2, "%s+ New topic...",
                             fidx == sel ? "> " : "  ");
                    attroff(A_BOLD);
                } else {
                    size_t cnt = count_entries_for_topic(topics[tidx]);
                    mvprintw(y, start_x + 2, "%s%s (%zu)",
                             fidx == sel ? "> " : "  ",
                             topics[tidx], cnt);
                }
                if (fidx == sel) { attroff(COLOR_PAIR(CP_POPUP_SEL)); attron(COLOR_PAIR(CP_POPUP)); }
            }
        }
        attroff(COLOR_PAIR(CP_POPUP));

        refresh();

        int key = getch();
        switch (key) {
            case 'j':
            case KEY_DOWN:
                if (fcount > 0 && sel < (int)fcount - 1) {
                    sel++;
                    if (sel >= scroll + visible) scroll++;
                }
                break;
            case 'k':
            case KEY_UP:
                if (sel > 0) {
                    sel--;
                    if (sel < scroll) scroll--;
                }
                break;
            case '\n':
            case '\r':
                if (fcount > 0) {
                    int tidx = filtered[sel];
                    if (tidx == -1) {
                        /* "New..." selected — show text input for new topic name */
                        mvhline(start_y + 3, start_x + 1, ' ', popup_w - 2);
                        attron(A_BOLD | COLOR_PAIR(CP_TAG));
                        mvprintw(start_y + 3, start_x + 2, "New topic: ");
                        attroff(A_BOLD | COLOR_PAIR(CP_TAG));
                        char new_name[128] = "";
                        int nlen = 0;
                        curs_set(1);
                        while (1) {
                            mvhline(start_y + 3, start_x + 13, ' ', popup_w - 14);
                            mvprintw(start_y + 3, start_x + 13, "%s", new_name);
                            refresh();
                            int nk = getch();
                            if (nk == '\n' || nk == '\r') {
                                if (nlen > 0) result = kb_strdup(new_name);
                                break;
                            } else if (nk == 27 || nk == CTRL('c')) {
                                break;
                            } else if (nk == KEY_BACKSPACE || nk == 127 || nk == 8) {
                                if (nlen > 0) new_name[--nlen] = '\0';
                            } else if (nk >= 32 && nk < 127 && nlen < (int)sizeof(new_name) - 1) {
                                new_name[nlen++] = (char)nk;
                                new_name[nlen] = '\0';
                            }
                        }
                        curs_set(0);
                        goto topic_done;
                    } else {
                        result = kb_strdup(topics[tidx]);
                    }
                }
                goto topic_done;
            case 27:   /* Escape */
            case CTRL('c'):
                goto topic_done;
            case KEY_BACKSPACE:
            case 127:
            case 8:
                /* Delete last char from filter */
                if (filter_len > 0) {
                    filter[--filter_len] = '\0';
                    /* Rebuild filtered list — always include "New..." */
                    fcount = 0;
                    filtered[fcount++] = -1;
                    for (size_t i = 0; i < topic_count; i++) {
                        if (filter_len == 0) {
                            filtered[fcount++] = (int)i;
                        } else {
                            /* Case-insensitive substring match */
                            const char *hay = topics[i];
                            size_t hlen = strlen(hay);
                            size_t flen = (size_t)filter_len;
                            int found = 0;
                            for (size_t h = 0; h + flen <= hlen; h++) {
                                int ok = 1;
                                for (size_t f = 0; f < flen; f++) {
                                    if (tolower((unsigned char)hay[h+f]) != tolower((unsigned char)filter[f])) {
                                        ok = 0; break;
                                    }
                                }
                                if (ok) { found = 1; break; }
                            }
                            if (found) filtered[fcount++] = (int)i;
                        }
                    }
                    sel = 0;
                    scroll = 0;
                }
                break;
            default:
                /* Printable characters → append to filter */
                if (key >= 32 && key < 127 && filter_len < (int)sizeof(filter) - 1) {
                    filter[filter_len++] = (char)key;
                    filter[filter_len] = '\0';
                    /* Rebuild filtered list — always include "New..." */
                    fcount = 0;
                    filtered[fcount++] = -1;
                    size_t flen = (size_t)filter_len;
                    for (size_t i = 0; i < topic_count; i++) {
                        const char *hay = topics[i];
                        size_t hlen = strlen(hay);
                        int found = 0;
                        for (size_t h = 0; h + flen <= hlen; h++) {
                            int ok = 1;
                            for (size_t f = 0; f < flen; f++) {
                                if (tolower((unsigned char)hay[h+f]) != tolower((unsigned char)filter[f])) {
                                    ok = 0; break;
                                }
                            }
                            if (ok) { found = 1; break; }
                        }
                        if (found) filtered[fcount++] = (int)i;
                    }
                    sel = 0;
                    scroll = 0;
                }
                break;
        }
    }

topic_done:
    free(filtered);
    for (size_t i = 0; i < topic_count; i++) free(topics[i]);
    free(topics);
    return result;
}

/* Tag selection popup with search and "New tag..." option.
 * Returns selected tag (strdup'd) or NULL on cancel. */
static char *do_tag_select_popup(void) {
    size_t tag_count = 0;
    char **tags = collect_unique_tags(&tag_count);

    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);

    int popup_w = 40;
    for (size_t i = 0; i < tag_count; i++) {
        int len = (int)strlen(tags[i]) + 6;
        if (len + 4 > popup_w) popup_w = len + 4;
    }
    if (popup_w > max_x - 4) popup_w = max_x - 4;

    /* Search filter state */
    char filter[128] = "";
    int filter_len = 0;

    /* Filtered indices into tags[] — index -1 means the "New..." entry */
    size_t alloc_count = tag_count + 1;
    int *filtered = kb_malloc(alloc_count * sizeof(int));
    size_t fcount = tag_count + 1;
    filtered[0] = -1;  /* "New..." entry always first */
    for (size_t i = 0; i < tag_count; i++) filtered[i + 1] = (int)i;

    int sel = 0;
    int scroll = 0;
    char *result = NULL;

    while (1) {
        int popup_h = (int)fcount + 6;
        if (popup_h > max_y - 4) popup_h = max_y - 4;
        if (popup_h < 7) popup_h = 7;
        int visible = popup_h - 6;

        int start_y = (max_y - popup_h) / 2;
        int start_x = (max_x - popup_w) / 2;

        /* Draw popup background */
        for (int y = start_y; y < start_y + popup_h; y++)
            mvhline(y, start_x, ' ', popup_w);

        /* Draw border */
        attron(A_BOLD | COLOR_PAIR(CP_HEADER));
        ui_hline(start_y, start_x, popup_w);
        ui_hline(start_y + popup_h - 1, start_x, popup_w);
        for (int y = start_y; y < start_y + popup_h; y++) {
            mvaddstr(y, start_x, "│");
            mvaddstr(y, start_x + popup_w - 1, "│");
        }
        mvaddstr(start_y, start_x, "┌");
        mvaddstr(start_y, start_x + popup_w - 1, "┐");
        mvaddstr(start_y + popup_h - 1, start_x, "└");
        mvaddstr(start_y + popup_h - 1, start_x + popup_w - 1, "┘");

        /* Title */
        mvprintw(start_y + 1, start_x + 2, "Select Tag:");
        attroff(A_BOLD | COLOR_PAIR(CP_HEADER));

        /* Separator below title */
        attron(COLOR_PAIR(CP_HEADER));
        ui_hline(start_y + 2, start_x + 1, popup_w - 2);
        attroff(COLOR_PAIR(CP_HEADER));

        /* Search bar */
        mvhline(start_y + 3, start_x + 1, ' ', popup_w - 2);
        attron(COLOR_PAIR(CP_TAG));
        mvprintw(start_y + 3, start_x + 2, "/ %s", filter);
        attroff(COLOR_PAIR(CP_TAG));

        /* Separator below search */
        attron(COLOR_PAIR(CP_HEADER));
        ui_hline(start_y + 4, start_x + 1, popup_w - 2);
        attroff(COLOR_PAIR(CP_HEADER));

        /* Draw filtered tag list */
        if (fcount == 0) {
            attron(A_DIM);
            mvprintw(start_y + 5, start_x + 4, "(no match)");
            attroff(A_DIM);
        } else {
            for (int i = 0; i < visible && (size_t)(scroll + i) < fcount; i++) {
                int fidx = scroll + i;
                int tidx = filtered[fidx];
                int y = start_y + 5 + i;

                if (fidx == sel) {
                    attron(COLOR_PAIR(CP_HIGHLIGHT));
                    mvhline(y, start_x + 1, ' ', popup_w - 2);
                }

                if (tidx == -1) {
                    /* "New..." entry */
                    attron(A_BOLD | COLOR_PAIR(fidx == sel ? CP_HIGHLIGHT : CP_TAG));
                    mvprintw(y, start_x + 2, "%s+ New tag...",
                             fidx == sel ? "> " : "  ");
                    attroff(A_BOLD | COLOR_PAIR(fidx == sel ? CP_HIGHLIGHT : CP_TAG));
                } else {
                    mvprintw(y, start_x + 2, "%s%s",
                             fidx == sel ? "> " : "  ",
                             tags[tidx]);
                }

                if (fidx == sel)
                    attroff(COLOR_PAIR(CP_HIGHLIGHT));
            }
        }

        refresh();

        int key = getch();
        switch (key) {
            case 'j':
            case KEY_DOWN:
                if (fcount > 0 && sel < (int)fcount - 1) {
                    sel++;
                    if (sel >= scroll + visible) scroll++;
                }
                break;
            case 'k':
            case KEY_UP:
                if (sel > 0) {
                    sel--;
                    if (sel < scroll) scroll--;
                }
                break;
            case '\n':
            case '\r':
                if (fcount > 0) {
                    int tidx = filtered[sel];
                    if (tidx == -1) {
                        /* "New..." selected — show text input */
                        mvhline(start_y + 3, start_x + 1, ' ', popup_w - 2);
                        attron(A_BOLD | COLOR_PAIR(CP_TAG));
                        mvprintw(start_y + 3, start_x + 2, "New tag: ");
                        attroff(A_BOLD | COLOR_PAIR(CP_TAG));
                        char new_name[128] = "";
                        int nlen = 0;
                        curs_set(1);
                        while (1) {
                            mvhline(start_y + 3, start_x + 11, ' ', popup_w - 12);
                            mvprintw(start_y + 3, start_x + 11, "%s", new_name);
                            refresh();
                            int nk = getch();
                            if (nk == '\n' || nk == '\r') {
                                if (nlen > 0) result = kb_strdup(new_name);
                                break;
                            } else if (nk == 27 || nk == CTRL('c')) {
                                break;
                            } else if (nk == KEY_BACKSPACE || nk == 127 || nk == 8) {
                                if (nlen > 0) new_name[--nlen] = '\0';
                            } else if (nk >= 32 && nk < 127 && nlen < (int)sizeof(new_name) - 1) {
                                new_name[nlen++] = (char)nk;
                                new_name[nlen] = '\0';
                            }
                        }
                        curs_set(0);
                        goto tag_select_done;
                    } else {
                        result = kb_strdup(tags[tidx]);
                    }
                }
                goto tag_select_done;
            case 27:   /* Escape */
            case CTRL('c'):
                goto tag_select_done;
            case KEY_BACKSPACE:
            case 127:
            case 8:
                /* Delete last char from filter */
                if (filter_len > 0) {
                    filter[--filter_len] = '\0';
                    fcount = 0;
                    filtered[fcount++] = -1;
                    for (size_t i = 0; i < tag_count; i++) {
                        if (filter_len == 0) {
                            filtered[fcount++] = (int)i;
                        } else {
                            const char *hay = tags[i];
                            size_t hlen = strlen(hay);
                            size_t flen = (size_t)filter_len;
                            int found = 0;
                            for (size_t h = 0; h + flen <= hlen; h++) {
                                int ok = 1;
                                for (size_t f = 0; f < flen; f++) {
                                    if (tolower((unsigned char)hay[h+f]) != tolower((unsigned char)filter[f])) {
                                        ok = 0; break;
                                    }
                                }
                                if (ok) { found = 1; break; }
                            }
                            if (found) filtered[fcount++] = (int)i;
                        }
                    }
                    sel = 0;
                    scroll = 0;
                }
                break;
            default:
                /* Printable characters → append to filter */
                if (key >= 32 && key < 127 && filter_len < (int)sizeof(filter) - 1) {
                    filter[filter_len++] = (char)key;
                    filter[filter_len] = '\0';
                    fcount = 0;
                    filtered[fcount++] = -1;
                    size_t flen = (size_t)filter_len;
                    for (size_t i = 0; i < tag_count; i++) {
                        const char *hay = tags[i];
                        size_t hlen = strlen(hay);
                        int found = 0;
                        for (size_t h = 0; h + flen <= hlen; h++) {
                            int ok = 1;
                            for (size_t f = 0; f < flen; f++) {
                                if (tolower((unsigned char)hay[h+f]) != tolower((unsigned char)filter[f])) {
                                    ok = 0; break;
                                }
                            }
                            if (ok) { found = 1; break; }
                        }
                        if (found) filtered[fcount++] = (int)i;
                    }
                    sel = 0;
                    scroll = 0;
                }
                break;
        }
    }

tag_select_done:
    free(filtered);
    for (size_t i = 0; i < tag_count; i++) free(tags[i]);
    free(tags);
    return result;
}

/* Multi-select tag filter popup.
 * Shows all tags with [x]/[ ] toggle. Space/Enter toggles, q/Esc applies.
 * Pre-selects currently active tag filters.
 * Sets tag_filters via set_tag_filters() on exit. */
static void do_tag_filter_popup(void) {
    size_t tag_count = 0;
    char **tags = collect_unique_tags(&tag_count);
    if (!tags) return;

    /* Selection state: which tags are checked */
    bool *checked = kb_malloc(tag_count * sizeof(bool));
    for (size_t i = 0; i < tag_count; i++) {
        checked[i] = false;
        for (size_t f = 0; f < g_tui.tag_filter_count; f++) {
            if (strcmp(tags[i], g_tui.tag_filters[f]) == 0) {
                checked[i] = true;
                break;
            }
        }
    }

    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);

    int popup_w = 44;
    for (size_t i = 0; i < tag_count; i++) {
        int len = (int)strlen(tags[i]) + 8;  /* [x] + tag + padding */
        if (len + 4 > popup_w) popup_w = len + 4;
    }
    if (popup_w > max_x - 4) popup_w = max_x - 4;

    /* Search filter state */
    char filter[128] = "";
    int filter_len = 0;

    /* Filtered indices into tags[] */
    int *filtered = kb_malloc(tag_count * sizeof(int));
    size_t fcount = tag_count;
    for (size_t i = 0; i < tag_count; i++) filtered[i] = (int)i;

    int sel = 0;
    int scroll = 0;

    while (1) {
        /* Count selected */
        size_t sel_count = 0;
        for (size_t i = 0; i < tag_count; i++)
            if (checked[i]) sel_count++;

        int popup_h = (int)fcount + 7;  /* border + title + sep + search + sep + items + border */
        if (popup_h > max_y - 4) popup_h = max_y - 4;
        if (popup_h < 8) popup_h = 8;
        int visible = popup_h - 7;

        int start_y = (max_y - popup_h) / 2;
        int start_x = (max_x - popup_w) / 2;

        /* Draw popup background */
        for (int y = start_y; y < start_y + popup_h; y++)
            mvhline(y, start_x, ' ', popup_w);

        /* Border */
        attron(A_BOLD | COLOR_PAIR(CP_HEADER));
        ui_hline(start_y, start_x, popup_w);
        ui_hline(start_y + popup_h - 1, start_x, popup_w);
        for (int y = start_y; y < start_y + popup_h; y++) {
            mvaddstr(y, start_x, "│");
            mvaddstr(y, start_x + popup_w - 1, "│");
        }
        mvaddstr(start_y, start_x, "┌");
        mvaddstr(start_y, start_x + popup_w - 1, "┐");
        mvaddstr(start_y + popup_h - 1, start_x, "└");
        mvaddstr(start_y + popup_h - 1, start_x + popup_w - 1, "┘");

        /* Title */
        mvprintw(start_y + 1, start_x + 2, "Filter Tags (%zu selected):", sel_count);
        attroff(A_BOLD | COLOR_PAIR(CP_HEADER));

        /* Separator */
        attron(COLOR_PAIR(CP_HEADER));
        ui_hline(start_y + 2, start_x + 1, popup_w - 2);
        attroff(COLOR_PAIR(CP_HEADER));

        /* Search bar */
        mvhline(start_y + 3, start_x + 1, ' ', popup_w - 2);
        attron(COLOR_PAIR(CP_TAG));
        mvprintw(start_y + 3, start_x + 2, "/ %s", filter);
        attroff(COLOR_PAIR(CP_TAG));

        /* Separator */
        attron(COLOR_PAIR(CP_HEADER));
        ui_hline(start_y + 4, start_x + 1, popup_w - 2);
        attroff(COLOR_PAIR(CP_HEADER));

        /* Tag list */
        if (fcount == 0) {
            attron(A_DIM);
            mvprintw(start_y + 5, start_x + 4, "(no match)");
            attroff(A_DIM);
        } else {
            for (int i = 0; i < visible && (size_t)(scroll + i) < fcount; i++) {
                int fidx = scroll + i;
                int tidx = filtered[fidx];
                int y = start_y + 5 + i;

                if (fidx == sel) {
                    attron(COLOR_PAIR(CP_HIGHLIGHT));
                    mvhline(y, start_x + 1, ' ', popup_w - 2);
                }

                mvprintw(y, start_x + 2, "%s[%c] %s",
                         fidx == sel ? "> " : "  ",
                         checked[tidx] ? 'x' : ' ',
                         tags[tidx]);

                if (fidx == sel)
                    attroff(COLOR_PAIR(CP_HIGHLIGHT));
            }
        }

        /* Help bar */
        attron(A_DIM);
        ui_hline(start_y + popup_h - 2, start_x + 1, popup_w - 2);
        mvprintw(start_y + popup_h - 1, start_x + 2,
                 "Space:toggle  Enter:apply  c:clear  Esc:cancel");
        attroff(A_DIM);

        refresh();

        int key = getch();
        switch (key) {
            case 'j': case KEY_DOWN:
                if (fcount > 0 && sel < (int)fcount - 1) {
                    sel++;
                    if (sel >= scroll + visible) scroll++;
                }
                break;
            case 'k': case KEY_UP:
                if (sel > 0) {
                    sel--;
                    if (sel < scroll) scroll--;
                }
                break;
            case ' ':
                /* Toggle selected tag */
                if (fcount > 0 && sel < (int)fcount) {
                    int tidx = filtered[sel];
                    checked[tidx] = !checked[tidx];
                }
                break;
            case 'c':
                /* Clear all */
                for (size_t i = 0; i < tag_count; i++) checked[i] = false;
                break;
            case '\n': case '\r': {
                /* Apply selection */
                size_t cnt = 0;
                for (size_t i = 0; i < tag_count; i++)
                    if (checked[i]) cnt++;
                if (cnt == 0) {
                    clear_tag_filters();
                } else {
                    char **sel_tags = kb_malloc(cnt * sizeof(char *));
                    size_t si = 0;
                    for (size_t i = 0; i < tag_count; i++) {
                        if (checked[i])
                            sel_tags[si++] = kb_strdup(tags[i]);
                    }
                    set_tag_filters(sel_tags, cnt);
                }
                goto tag_filter_done;
            }
            case 27: case CTRL('c'):
                /* Cancel — no changes */
                goto tag_filter_done;
            case KEY_BACKSPACE: case 127: case 8:
                if (filter_len > 0) {
                    filter[--filter_len] = '\0';
                    fcount = 0;
                    for (size_t i = 0; i < tag_count; i++) {
                        if (filter_len == 0) {
                            filtered[fcount++] = (int)i;
                        } else {
                            const char *hay = tags[i];
                            size_t hlen = strlen(hay);
                            size_t flen = (size_t)filter_len;
                            int found = 0;
                            for (size_t h = 0; h + flen <= hlen; h++) {
                                int ok = 1;
                                for (size_t f = 0; f < flen; f++) {
                                    if (tolower((unsigned char)hay[h+f]) != tolower((unsigned char)filter[f])) {
                                        ok = 0; break;
                                    }
                                }
                                if (ok) { found = 1; break; }
                            }
                            if (found) filtered[fcount++] = (int)i;
                        }
                    }
                    sel = 0; scroll = 0;
                }
                break;
            default:
                if (key >= 32 && key < 127 && filter_len < (int)sizeof(filter) - 1) {
                    filter[filter_len++] = (char)key;
                    filter[filter_len] = '\0';
                    fcount = 0;
                    size_t flen = (size_t)filter_len;
                    for (size_t i = 0; i < tag_count; i++) {
                        const char *hay = tags[i];
                        size_t hlen = strlen(hay);
                        int found = 0;
                        for (size_t h = 0; h + flen <= hlen; h++) {
                            int ok = 1;
                            for (size_t f = 0; f < flen; f++) {
                                if (tolower((unsigned char)hay[h+f]) != tolower((unsigned char)filter[f])) {
                                    ok = 0; break;
                                }
                            }
                            if (ok) { found = 1; break; }
                        }
                        if (found) filtered[fcount++] = (int)i;
                    }
                    sel = 0; scroll = 0;
                }
                break;
        }
    }

tag_filter_done:
    free(filtered);
    free(checked);
    for (size_t i = 0; i < tag_count; i++) free(tags[i]);
    free(tags);
}

static void draw_left_pane(void) {
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);
    (void)max_x;

    int start_y = 2;
    int height = max_y - start_y - 1;

    /* Clear pane */
    for (int y = start_y; y < max_y - 1; y++) {
        mvwhline(stdscr, y, 0, ' ', g_tui.left_width);
    }

    /* Draw entries */
    size_t count = effective_count();

    int archive_mode = g_tui.archive_mode;

    char prev_repo[128] = "";
    char prev_topic[128] = "";
    char prev_date[16] = "";
    int y = start_y;

    for (int idx = g_tui.offset; y < max_y - 1 && (size_t)idx < count; idx++) {
        KB_Entry *entry = effective_entry_at(idx);
        if (!entry) continue;

        if (archive_mode) {
            /* Archive mode: group by date (newest first) */
            char date_label[16] = "";
            if (entry->timestamp > 0) {
                struct tm *tm = localtime(&entry->timestamp);
                if (tm) snprintf(date_label, sizeof(date_label),
                                 "%02d-%02d", tm->tm_mon + 1, tm->tm_mday);
            }

            if (date_label[0] && strcmp(date_label, prev_date) != 0) {
                if (y < max_y - 1) {
                    /* Draw date header: ── 04-14 ──── */
                    attron(A_BOLD | COLOR_PAIR(CP_HEADER));
                    mvhline(y, 0, ' ', g_tui.left_width);
                    mvprintw(y, 1, "── %s ", date_label);

                    /* Fill rest with ── */
                    int pos = 7 + (int)strlen(date_label);
                    for (int x = pos; x < g_tui.left_width - 1; x++)
                        mvaddstr(y, x, "─");

                    attroff(A_BOLD | COLOR_PAIR(CP_HEADER));
                    y++;
                }
                strncpy(prev_date, date_label, sizeof(prev_date) - 1);
            }
        } else {
            /* Knowledge mode: group by date, show repo/topic tag on each entry */
            char date_label[16] = "";
            if (entry->timestamp > 0) {
                struct tm *tm = localtime(&entry->timestamp);
                if (tm) snprintf(date_label, sizeof(date_label),
                                 "%02d-%02d", tm->tm_mon + 1, tm->tm_mday);
            }

            if (date_label[0] && strcmp(date_label, prev_date) != 0) {
                if (y < max_y - 1) {
                    attron(A_BOLD | COLOR_PAIR(CP_HEADER));
                    mvhline(y, 0, ' ', g_tui.left_width);
                    mvprintw(y, 1, "── %s ", date_label);
                    int pos = 7 + (int)strlen(date_label);
                    for (int fx = pos; fx < g_tui.left_width - 1; fx++)
                        mvaddstr(y, fx, "─");
                    attroff(A_BOLD | COLOR_PAIR(CP_HEADER));
                    y++;
                }
                strncpy(prev_date, date_label, sizeof(prev_date) - 1);
            }
        }

        if (y >= max_y - 1) break;

        int is_selected = (idx == g_tui.selected);
        if (is_selected) attron(COLOR_PAIR(CP_HIGHLIGHT));

        /* Truncate title */
        char title[256];
        strncpy(title, entry->title ? entry->title : "Untitled", sizeof(title) - 1);
        title[sizeof(title) - 1] = '\0';

        int max_title_len = g_tui.left_width - 6;
        if ((int)strlen(title) > max_title_len) {
            title[max_title_len - 2] = '.';
            title[max_title_len - 1] = '.';
            title[max_title_len] = '\0';
        }

        /* Clear the row first */
        mvhline(y, 1, ' ', g_tui.left_width - 1);

        /* Draw mark indicator for multi-select */
        uint64_t eid = entry->id;
        if (g_tui.marked && eid < g_tui.index->entry_capacity && g_tui.marked[eid]) {
            if (is_selected) attroff(COLOR_PAIR(CP_HIGHLIGHT));
            attron(COLOR_PAIR(CP_TAG) | A_BOLD);
            mvprintw(y, 1, "●");
            attroff(COLOR_PAIR(CP_TAG) | A_BOLD);
            if (is_selected) attron(COLOR_PAIR(CP_HIGHLIGHT));
        }

        /* Knowledge mode: show repo/topic tag before title */
        int title_x = 4;
        if (!archive_mode && entry->path) {
            char repo[128];
            get_topic_from_path(entry->path, repo, sizeof(repo));
            if (!repo[0]) get_repo_from_path(entry->path, repo, sizeof(repo));
            if (repo[0]) {
                if (is_selected) attroff(COLOR_PAIR(CP_HIGHLIGHT));
                attron(COLOR_PAIR(CP_TAG) | A_DIM);
                mvprintw(y, 2, "%s", repo);
                attroff(COLOR_PAIR(CP_TAG) | A_DIM);
                if (is_selected) attron(COLOR_PAIR(CP_HIGHLIGHT));
                title_x = 2 + (int)strlen(repo) + 1;
                if (title_x > g_tui.left_width / 2) title_x = g_tui.left_width / 2;
                max_title_len = g_tui.left_width - title_x - 2;
                if ((int)strlen(title) > max_title_len && max_title_len > 2) {
                    title[max_title_len - 2] = '.';
                    title[max_title_len - 1] = '.';
                    title[max_title_len] = '\0';
                }
            }
        }

        /* Show [draft] prefix for inbox/draft notes */
        if (entry->status && strcmp(entry->status, "draft") == 0) {
            if (is_selected) attroff(COLOR_PAIR(CP_HIGHLIGHT));
            attron(COLOR_PAIR(CP_QUOTE) | A_BOLD);
            mvprintw(y, title_x, "[draft]");
            attroff(COLOR_PAIR(CP_QUOTE) | A_BOLD);
            if (is_selected) attron(COLOR_PAIR(CP_HIGHLIGHT));
            title_x += 8;
            max_title_len -= 8;
            if ((int)strlen(title) > max_title_len && max_title_len > 2) {
                title[max_title_len - 2] = '.';
                title[max_title_len - 1] = '.';
                title[max_title_len] = '\0';
            }
        }

        /* Draw title */
        draw_title_highlighted(title, g_tui.search_query, y, title_x, max_title_len, is_selected);

        if (is_selected) attroff(COLOR_PAIR(CP_HIGHLIGHT));
        y++;
    }

    /* Draw separator */
    ui_vline(start_y, g_tui.left_width, height);
}

/* Check if line starts/ends a code block */
static int check_code_fence(const char *line) {
    return strncmp(line, "```", 3) == 0;
}

/* Render a horizontal rule */
static int is_horizontal_rule(const char *line) {
    int len = strlen(line);
    int dashes = 0, stars = 0, underscores = 0;

    for (int i = 0; i < len; i++) {
        if (line[i] == '-') dashes++;
        else if (line[i] == '*') stars++;
        else if (line[i] == '_') underscores++;
        else if (line[i] != ' ' && line[i] != '\t') return 0;
    }

    return (dashes >= 3 || stars >= 3 || underscores >= 3);
}

/* Check if line is a list item */
static int get_list_indent(const char *line) {
    const char *p = line;
    int spaces = 0;

    while (*p == ' ') {
        spaces++;
        p++;
    }

    /* Unordered list: -, *, + */
    if ((*p == '-' || *p == '*' || *p == '+') && p[1] == ' ') {
        return spaces;
    }

    /* Ordered list: 1., 2), etc. */
    if (isdigit(*p)) {
        while (isdigit(*p)) p++;
        if ((*p == '.' || *p == ')') && p[1] == ' ') {
            return spaces;
        }
    }

    return -1;
}

/* Check if line is a checkbox item */
static int is_checkbox(const char *line, int *checked) {
    const char *p = line;
    while (*p == ' ') p++;

    /* Check for - [ ] or - [x] or - [X] */
    if (*p == '-' && p[1] == ' ' && p[2] == '[' &&
        (p[3] == ' ' || p[3] == 'x' || p[3] == 'X') && p[4] == ']') {
        *checked = (p[3] == 'x' || p[3] == 'X');
        return 1;
    }
    return 0;
}

/* Check if line is a table row */
static int is_table_row(const char *line) {
    const char *p = line;
    while (*p == ' ') p++;
    return (*p == '|');
}

/* Check if line is a table separator row (|---|:--|--:|:--:|) */
static int is_table_separator(const char *line) {
    while (*line == ' ') line++;
    if (*line == '|') line++;
    int has_dash = 0;
    while (*line && *line != '\n' && *line != '\r') {
        if (*line == '-') has_dash = 1;
        else if (*line != '|' && *line != ':' && *line != ' ') return 0;
        line++;
    }
    return has_dash;
}

/* Check if line is a blockquote */
static int is_blockquote(const char *line) {
    const char *p = line;
    while (*p == ' ') p++;
    return (*p == '>');
}

/* Emit one UTF-8 character via addnstr so ncursesw composes the full
 * codepoint.  Advances *ptr past the character and increments *col by 1. */
static void emit_utf8(const char **ptr, int *col, int max_col) {
    unsigned char c = (unsigned char)**ptr;
    int n = (c < 0x80) ? 1 : (c < 0xE0) ? 2 : (c < 0xF0) ? 3 : 4;

    /* CJK and wide characters occupy 2 columns */
    int char_width = 1;
    if (n >= 3) {
        /* Decode UTF-8 codepoint to check width */
        unsigned int cp = 0;
        if (n == 3) cp = ((c & 0x0F) << 12) | ((((unsigned char)(*ptr)[1]) & 0x3F) << 6) | (((unsigned char)(*ptr)[2]) & 0x3F);
        else if (n == 4) cp = ((c & 0x07) << 18) | ((((unsigned char)(*ptr)[1]) & 0x3F) << 12) | ((((unsigned char)(*ptr)[2]) & 0x3F) << 6) | (((unsigned char)(*ptr)[3]) & 0x3F);
        /* CJK Unified Ideographs, CJK symbols, Hiragana, Katakana, Hangul, fullwidth */
        if ((cp >= 0x2E80 && cp <= 0x9FFF) || (cp >= 0xAC00 && cp <= 0xD7AF) ||
            (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFE30 && cp <= 0xFE4F) ||
            (cp >= 0xFF00 && cp <= 0xFF60) || (cp >= 0xFFE0 && cp <= 0xFFE6) ||
            (cp >= 0x20000 && cp <= 0x2FA1F))
            char_width = 2;
    }

    /* Clip: skip character if it would overflow the boundary */
    if (*col + char_width > max_col) {
        *ptr += n;
        return;
    }

    addnstr(*ptr, n);
    *ptr += n;
    *col += char_width;
}

/* Advanced markdown rendering - returns new in_code_block state */
static int render_markdown_line(const char *line, int y, int x, int width, int in_code_block) {
    if (!line) return in_code_block;

    int col = x;
    int max_col = x + width;  /* hard right boundary for clipping */
    const char *p = line;
    int in_code = in_code_block;

    /* Conversation role headers - render as full-width colored bars */
    if (strncmp(line, "## Human", 8) == 0 &&
        (line[8] == '\0' || line[8] == '\n' || line[8] == '\r' || line[8] == ' ')) {
        attron(A_BOLD | COLOR_PAIR(CP_HUMAN));
        mvhline(y, x, ' ', width);
        mvprintw(y, x + 1, " Human");
        attroff(A_BOLD | COLOR_PAIR(CP_HUMAN));
        return in_code;
    }
    if (strncmp(line, "## Claude", 9) == 0 &&
        (line[9] == '\0' || line[9] == '\n' || line[9] == '\r' || line[9] == ' ')) {
        attron(A_BOLD | COLOR_PAIR(CP_CLAUDE));
        mvhline(y, x, ' ', width);
        mvprintw(y, x + 1, " Claude");
        attroff(A_BOLD | COLOR_PAIR(CP_CLAUDE));
        return in_code;
    }

    /* Code fence: opening or closing - render with box borders */
    if (check_code_fence(line)) {
        if (!in_code) {
            /* Opening fence - top border with language label */
            in_code = 1;
            attron(COLOR_PAIR(CP_CODE_BORDER));
            mvaddstr(y, x, "┌");
            col = x + 1;
            if (col < max_col) { addstr("─"); col++; }
            /* Extract language after ``` */
            const char *lp = line;
            while (*lp == '`' || *lp == '~') lp++;
            while (*lp == ' ') lp++;
            if (*lp && *lp != '\n' && *lp != '\r') {
                if (col < max_col) { addch(' '); col++; }
                attron(A_BOLD);
                while (*lp && *lp != '\n' && *lp != '\r' && *lp != ' ' && col < max_col) {
                    addch(*lp++); col++;
                }
                attroff(A_BOLD);
                if (col < max_col) { addch(' '); col++; }
            }
            int cy, cx;
            getyx(stdscr, cy, cx);
            (void)cy;
            if (cx < max_col) ui_hline(y, cx, max_col - cx);
            attroff(COLOR_PAIR(CP_CODE_BORDER));
        } else {
            /* Closing fence - bottom border */
            in_code = 0;
            attron(COLOR_PAIR(CP_CODE_BORDER));
            mvaddstr(y, x, "└");
            int cy, cx;
            getyx(stdscr, cy, cx);
            (void)cy;
            if (cx < max_col) ui_hline(y, cx, max_col - cx);
            attroff(COLOR_PAIR(CP_CODE_BORDER));
        }
        return in_code;
    }

    /* Code block content - left border */
    if (in_code) {
        attron(COLOR_PAIR(CP_CODE_BORDER));
        mvaddstr(y, x, "│");
        attroff(COLOR_PAIR(CP_CODE_BORDER));
        col = x + 1;
        addch(' ');
        col++;
        attron(COLOR_PAIR(CP_CODE));
        while (*p && col < max_col) {
            emit_utf8(&p, &col, max_col);
        }
        attroff(COLOR_PAIR(CP_CODE));
        attrset(A_NORMAL);
        return in_code;
    }

    /* Handle horizontal rule - dimmed full-width line */
    if (is_horizontal_rule(line)) {
        attron(A_DIM | COLOR_PAIR(CP_HEADER));
        ui_hline(y, x, width);
        attroff(A_DIM | COLOR_PAIR(CP_HEADER));
        return in_code;
    }

    /* Handle list items with nesting */
    int list_indent = get_list_indent(line);
    if (list_indent >= 0) {
        int level = list_indent / 2;  /* 2 spaces per nesting level */
        int indent_cols = 2 + level * 2;  /* base 2 + 2 per level */
        /* Clamp indent to available width */
        if (indent_cols > width - 4) indent_cols = width > 4 ? width - 4 : 0;

        /* Check for checkbox */
        int checked = 0;
        if (is_checkbox(line, &checked)) {
            attron(COLOR_PAIR(CP_LIST));
            mvprintw(y, x, "%*s[%s] ", indent_cols, "", checked ? "x" : " ");
            attroff(COLOR_PAIR(CP_LIST));
            col = x + indent_cols + 4;
            /* Skip past - [x] */
            while (*p == ' ') p++;
            p += 5; /* skip "- [x]" or "- [ ]" */
            if (*p == ' ') p++;
        } else {
            attron(COLOR_PAIR(CP_LIST));
            /* Move past spaces and bullet/number */
            while (*p == ' ') p++;
            while (*p && *p != ' ') p++;
            if (*p == ' ') p++;
            /* Different bullet per nesting level */
            const char *bullets[] = {"\u2022", "\u25E6", "\u25B8", "\u00B7"};
            const char *bullet = bullets[level < 4 ? level : 3];
            mvprintw(y, x, "%*s%s ", indent_cols, "", bullet);
            attroff(COLOR_PAIR(CP_LIST));
            col = x + indent_cols + 2;
        }
        if (col >= max_col) col = max_col - 1;
        /* Position cursor for inline rendering of list content */
        move(y, col);
    }

    /* Handle table row */
    if (is_table_row(line)) {
        while (*p == ' ') p++;
        if (is_table_separator(p)) {
            /* Separator row: draw horizontal line */
            attron(COLOR_PAIR(CP_CODE_BORDER));
            while (*p && col < max_col) {
                if (*p == '|') {
                    addstr("┼");
                    col++;
                } else {
                    addstr("─");
                    col++;
                }
                p++;
            }
            attroff(COLOR_PAIR(CP_CODE_BORDER));
        } else {
            /* Data row: render cells with │ separators */
            attron(COLOR_PAIR(CP_CODE));
            while (*p && col < max_col) {
                if (*p == '|') {
                    attroff(COLOR_PAIR(CP_CODE));
                    attron(COLOR_PAIR(CP_CODE_BORDER));
                    addstr("│");
                    col++;
                    attroff(COLOR_PAIR(CP_CODE_BORDER));
                    attron(COLOR_PAIR(CP_CODE));
                    p++;
                } else {
                    emit_utf8(&p, &col, max_col);
                }
            }
            attroff(COLOR_PAIR(CP_CODE));
        }
        attrset(A_NORMAL);
        return in_code;
    }

    /* Handle blockquote with nesting */
    if (is_blockquote(line)) {
        while (*p == ' ') p++;
        int depth = 0;
        while (*p == '>') {
            depth++;
            p++;
            if (*p == ' ') p++;
        }
        /* Render one │ bar per nesting level */
        attron(COLOR_PAIR(CP_QUOTE));
        for (int d = 0; d < depth && x + d * 2 + 1 < max_col; d++) {
            mvprintw(y, x + d * 2, "\u2502 ");
        }
        attroff(COLOR_PAIR(CP_QUOTE));
        col = x + depth * 2;
        if (col >= max_col) col = max_col - 1;
        move(y, col);
    }

    /* Handle headers - require space after # markers */
    if (col == x && p[0] == '#') {
        int level = 0;
        while (p[level] == '#' && level < 6) level++;
        if (p[level] == ' ') {
            /* H1: bold + full-width underline marker */
            if (level == 1) {
                attron(A_BOLD | A_UNDERLINE | COLOR_PAIR(CP_HEADER));
                mvprintw(y, x, "\u2550\u2550 ");
                col += 3;
            }
            /* H2: bold + line marker */
            else if (level == 2) {
                attron(A_BOLD | COLOR_PAIR(CP_HEADER));
                mvprintw(y, x, "\u2500\u2500 ");
                col += 3;
            }
            /* H3: bold, slightly indented */
            else if (level == 3) {
                attron(A_BOLD | COLOR_PAIR(CP_HEADER));
                mvprintw(y, x, " \u25B6 ");
                col += 3;
            }
            /* H4: bold dim */
            else if (level == 4) {
                attron(A_BOLD | COLOR_PAIR(CP_EMPHASIS));
                mvprintw(y, x, " \u25B7 ");
                col += 3;
            }
            /* H5-H6: dim with marker */
            else {
                attron(COLOR_PAIR(CP_EMPHASIS));
                mvprintw(y, x, "  %.*s ", level, "######");
                col += level + 3;
            }
            p += level;
            while (*p == ' ') p++;
            /* Render header text with inline formatting */
            while (*p && col < max_col) {
                emit_utf8(&p, &col, max_col);
            }
            attrset(A_NORMAL);
            return in_code;
        }
    }

    /* Render inline elements */
    while (*p && col < max_col) {

        /* Inline code: `code` */
        if (*p == '`' && !(p > line && p[-1] == '`')) {
            attron(COLOR_PAIR(CP_CODE));
            p++;
            while (*p && *p != '`' && col < max_col) {
                emit_utf8(&p, &col, max_col);
            }
            attroff(COLOR_PAIR(CP_CODE));
            if (*p == '`') p++;
            continue;
        }

        /* Strikethrough: ~~text~~ */
        if (p[0] == '~' && p[1] == '~') {
            attron(A_DIM);
            p += 2;
            while (*p && !(p[0] == '~' && p[1] == '~') && col < max_col) {
                emit_utf8(&p, &col, max_col);
            }
            attroff(A_DIM);
            if (p[0] == '~' && p[1] == '~') p += 2;
            continue;
        }

        /* Bold and Italic: ***text*** */
        if (p[0] == '*' && p[1] == '*' && p[2] == '*' && p[3] != ' ' && p[3] != '\0') {
            /* Find closing *** */
            const char *end = strstr(p + 3, "***");
            if (end) {
                attron(A_BOLD | A_UNDERLINE);
                p += 3;
                while (p < end && col < max_col) {
                    emit_utf8(&p, &col, max_col);
                }
                attroff(A_BOLD | A_UNDERLINE);
                if (p == end) p += 3;
                continue;
            }
        }

        /* Bold: **text** */
        if (p[0] == '*' && p[1] == '*' && p[2] != ' ' && p[2] != '*' && p[2] != '\0') {
            /* Find closing ** (but not ***) */
            const char *end = p + 2;
            while (*end) {
                if (end[0] == '*' && end[1] == '*' && (end[2] != '*')) break;
                end++;
            }
            if (end[0] == '*' && end[1] == '*') {
                attron(A_BOLD);
                p += 2;
                while (p < end && col < max_col) {
                    emit_utf8(&p, &col, max_col);
                }
                attroff(A_BOLD);
                if (p == end) p += 2;
                continue;
            }
        }

        /* Italic: *text* or _text_ - require word boundary */
        if ((*p == '*' || *p == '_') && p[1] != ' ' && p[1] != '\0' && p[1] != *p) {
            char marker = *p;
            /* _ requires word boundary: prev must be space/start, next must be non-space */
            if (marker == '_' && p > line && p[-1] != ' ' && p[-1] != '\t') {
                emit_utf8(&p, &col, max_col);
                continue;
            }
            /* Find closing marker with word boundary check */
            const char *end = p + 1;
            while (*end && *end != marker) end++;
            if (*end == marker && end[-1] != ' ' &&
                (marker != '_' || !end[1] || end[1] == ' ' || end[1] == '.' ||
                 end[1] == ',' || end[1] == ')' || end[1] == '\0')) {
                attron(A_UNDERLINE | COLOR_PAIR(CP_EMPHASIS));
                p++;
                while (p < end && col < max_col) {
                    emit_utf8(&p, &col, max_col);
                }
                attroff(A_UNDERLINE | COLOR_PAIR(CP_EMPHASIS));
                if (p == end) p++;
                continue;
            }
        }

        /* Links: [text](url) */
        if (*p == '[') {
            const char *text_end = strchr(p, ']');
            const char *url_start = text_end ? strchr(text_end, '(') : NULL;
            const char *url_end = url_start ? strchr(url_start, ')') : NULL;

            if (text_end && url_start && url_end && url_start == text_end + 1) {
                attron(COLOR_PAIR(CP_LINK) | A_UNDERLINE);
                const char *tp = p + 1;
                while (tp < text_end && col < max_col) {
                    emit_utf8(&tp, &col, max_col);
                }
                attroff(COLOR_PAIR(CP_LINK) | A_UNDERLINE);
                p = url_end + 1;
                continue;
            }
        }

        /* Quick link: <url> */
        if (*p == '<' && p[1] != ' ') {
            const char *url_end = strchr(p + 1, '>');
            if (url_end && col + (url_end - p - 1) < max_col) {
                attron(COLOR_PAIR(CP_LINK) | A_UNDERLINE);
                const char *up = p + 1;
                while (up < url_end && col < max_col) {
                    emit_utf8(&up, &col, max_col);
                }
                attroff(COLOR_PAIR(CP_LINK) | A_UNDERLINE);
                p = url_end + 1;
                continue;
            }
        }

        /* Images: ![alt](url) - show alt text with indicator */
        if (p[0] == '!' && p[1] == '[') {
            const char *alt_end = strchr(p + 2, ']');
            const char *url_start = alt_end ? strchr(alt_end, '(') : NULL;
            const char *url_end = url_start ? strchr(url_start, ')') : NULL;

            if (alt_end && url_start && url_end) {
                attron(COLOR_PAIR(CP_LINK) | A_DIM);
                if (col < max_col) { addch('['); col++; }
                const char *ap = p + 2;
                while (ap < alt_end && col < max_col) {
                    emit_utf8(&ap, &col, max_col);
                }
                if (col < max_col) { addch(']'); col++; }
                attroff(COLOR_PAIR(CP_LINK) | A_DIM);
                p = url_end + 1;
                continue;
            }
        }

        /* Plain text */
        emit_utf8(&p, &col, max_col);
    }

    /* Reset attributes */
    attrset(A_NORMAL);

    return in_code;
}

/* Case-insensitive search for query in a line of raw text.
 * Returns byte offset of match, or -1. */
static int line_match_offset(const char *line, size_t line_len, const char *query) {
    if (!query || !*query) return -1;
    size_t qlen = strlen(query);
    if (qlen > line_len) return -1;

    char lo_q[256];
    strncpy(lo_q, query, sizeof(lo_q) - 1);
    lo_q[sizeof(lo_q) - 1] = '\0';
    for (char *c = lo_q; *c; c++) *c = (char)tolower((unsigned char)*c);

    for (size_t i = 0; i + qlen <= line_len; i++) {
        int ok = 1;
        for (size_t j = 0; j < qlen; j++) {
            if (tolower((unsigned char)line[i + j]) != (unsigned char)lo_q[j]) { ok = 0; break; }
        }
        if (ok) return (int)i;
    }
    return -1;
}

/* Build match line cache for a given entry and query */
static void build_match_cache(uint64_t entry_id, const char *content, const char *query) {
    /* Free old cache */
    free(g_tui.cached_match_lines);
    free(g_tui.cached_match_query);
    g_tui.cached_match_lines = NULL;
    g_tui.cached_match_query = NULL;
    g_tui.cached_match_count = 0;
    g_tui.cached_match_entry_id = 0;

    if (!content || !query || !*query) return;

    /* Collect all matching line numbers */
    int *matches = kb_malloc(8192 * sizeof(int));
    int nmatch = 0;
    const char *p = content;
    int line = 0;

    while (*p && nmatch < 8192) {
        const char *end = p;
        while (*end && *end != '\n') end++;
        size_t ll = (size_t)(end - p);
        if (line_match_offset(p, ll, query) >= 0)
            matches[nmatch++] = line;
        line++;
        p = *end ? end + 1 : end;
    }

    if (nmatch > 0) {
        g_tui.cached_match_lines = kb_realloc(matches, nmatch * sizeof(int));
        g_tui.cached_match_count = nmatch;
        g_tui.cached_match_entry_id = entry_id;
        g_tui.cached_match_query = kb_strdup(query);
    } else {
        free(matches);
    }
}

/* Check if match cache is valid for this entry and query */
static int match_cache_valid(uint64_t entry_id, const char *query) {
    return g_tui.cached_match_count > 0 &&
           g_tui.cached_match_entry_id == entry_id &&
           g_tui.cached_match_query && query &&
           strcmp(g_tui.cached_match_query, query) == 0;
}

/* Find next/prev match line using cached match results.
 * direction: +1 = next after from_line, -1 = prev before from_line.
 * Wraps around. Returns -1 if no match at all. */
static int find_match_line(const char *content, const char *query,
                            int from_line, int direction) {
    if (!content || !query || !*query) return -1;

    /* Use cached results. Caller is responsible for building cache before first call. */
    int nmatch = g_tui.cached_match_count;
    int *matches = g_tui.cached_match_lines;

    if (nmatch == 0 || !matches) return -1;

    if (direction >= 0) {
        for (int i = 0; i < nmatch; i++)
            if (matches[i] > from_line) return matches[i];
        return matches[0];  /* wrap */
    } else {
        for (int i = nmatch - 1; i >= 0; i--)
            if (matches[i] < from_line) return matches[i];
        return matches[nmatch - 1];  /* wrap */
    }
}

/* Get currently selected entry (list mode helper) */
static KB_Entry *get_preview_entry(void) {
    size_t count = effective_count();
    if (g_tui.selected < 0 || (size_t)g_tui.selected >= count) return NULL;
    return effective_entry_at(g_tui.selected);
}

/* Draw right pane (content preview) */
static void draw_right_pane(void) {
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);

    int start_y = 2;
    int height = max_y - start_y - 1;
    int x = g_tui.left_width + 2;
    int width = max_x - x - 1;

    /* Clear pane */
    for (int y = start_y; y < max_y - 1; y++) {
        mvwhline(stdscr, y, g_tui.left_width + 1, ' ', width);
    }

    /* Get selected entry */
    KB_Entry *entry = NULL;
    size_t count = effective_count();

    if (g_tui.selected >= 0 && (size_t)g_tui.selected < count) {
        entry = effective_entry_at(g_tui.selected);
    }

    if (!entry || !entry->raw_content) {
        mvprintw(start_y + height / 2, x + width / 2 - 10, "No content to display");
        return;
    }

    /* Render content line by line */
    const char *content = entry->raw_content;
    int line_num = 0;
    int y = start_y;
    int in_code = 0;  /* Track code block state across lines */

    while (*content && y < max_y - 1) {
        /* Find end of line */
        const char *line_end = content;
        while (*line_end && *line_end != '\n') line_end++;

        size_t line_len = (size_t)(line_end - content);

        /* Skip if before scroll offset */
        if (line_num >= g_tui.top_offset) {
            int is_match = (g_tui.search_query && *g_tui.search_query &&
                            line_match_offset(content, line_len, g_tui.search_query) >= 0);
            int is_current = (line_num == g_tui.preview_match_line);

            /* Gutter indicator */
            if (is_current) {
                attron(COLOR_PAIR(CP_TAG) | A_BOLD);
                mvaddch(y, g_tui.left_width + 1, '>');
                attroff(COLOR_PAIR(CP_TAG) | A_BOLD);
            } else if (is_match) {
                attron(COLOR_PAIR(CP_TAG));
                mvaddch(y, g_tui.left_width + 1, '-');
                attroff(COLOR_PAIR(CP_TAG));
            }

            /* Render line using stack buffer (no heap allocation) */
            {
                char stack_buf[4096];
                const char *line_ptr;
                if (line_len < sizeof(stack_buf)) {
                    memcpy(stack_buf, content, line_len);
                    stack_buf[line_len] = '\0';
                    line_ptr = stack_buf;
                } else {
                    /* Extremely long line: fall back to heap (rare) */
                    char *heap_buf = kb_strndup(content, line_len);
                    move(y, x);
                    in_code = render_markdown_line(heap_buf, y, x, width, in_code);
                    free(heap_buf);
                    goto line_rendered;
                }
                move(y, x);
                in_code = render_markdown_line(line_ptr, y, x, width, in_code);
            }
            line_rendered:

            /* Overlay highlight on current match line */
            if (is_current) {
                mvchgat(y, x, width, A_BOLD, CP_TAG, NULL);
            }

            y++;
        }

        line_num++;
        if (*line_end == '\n') content = line_end + 1;
        else content = line_end;
    }
}

/* Draw status bar */
static void draw_status_bar(void) {
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);

    attron(A_REVERSE);
    mvwhline(stdscr, max_y - 1, 0, ' ', max_x);

    KB_Entry *entry = NULL;
    size_t count = effective_count();

    if (g_tui.selected >= 0 && (size_t)g_tui.selected < count) {
        entry = effective_entry_at(g_tui.selected);
    }

    if (entry) {
        const char *path = entry->path ? entry->path : "";
        if (g_tui.loaded_mark_count > 0 && g_claude_pid > 0) {
            size_t new_count = g_tui.mark_count > g_tui.loaded_mark_count
                             ? g_tui.mark_count - g_tui.loaded_mark_count : 0;
            if (new_count > 0)
                mvprintw(max_y - 1, 1, "%zu loaded + %zu new | %s",
                         g_tui.loaded_mark_count, new_count, path);
            else
                mvprintw(max_y - 1, 1, "%zu loaded | %s",
                         g_tui.loaded_mark_count, path);
        } else if (g_tui.mark_count > 0) {
            mvprintw(max_y - 1, 1, "%zu selected | %s",
                     g_tui.mark_count, path);
        } else {
            mvprintw(max_y - 1, 1, "%s", path);
        }
    }

    attroff(A_REVERSE);
}

/* ============================================================================
 * INBOX REVIEW MODE
 * ============================================================================ */

/* Extract a YAML frontmatter field value from raw content.
 * Returns strdup'd string or NULL. Caller must free. */
static char *extract_fm_field(const char *raw, const char *field) {
    if (!raw || !field) return NULL;
    /* Only search within frontmatter (between first --- and second ---) */
    if (strncmp(raw, "---", 3) != 0) return NULL;
    const char *fm_end = strstr(raw + 3, "---");
    if (!fm_end) return NULL;

    char needle[64];
    snprintf(needle, sizeof(needle), "\n%s:", field);
    const char *hit = strstr(raw, needle);
    if (!hit || hit >= fm_end) return NULL;
    hit += strlen(needle);
    while (*hit == ' ') hit++;
    /* Strip quotes */
    char quote = 0;
    if (*hit == '"' || *hit == '\'') { quote = *hit; hit++; }
    const char *end = hit;
    while (*end && *end != '\n') end++;
    if (quote && end > hit && *(end - 1) == quote) end--;
    if (end <= hit) return NULL;
    return kb_strndup(hit, end - hit);
}

/* Update a YAML frontmatter field value in a file on disk.
 * Replaces "field: old_value" with "field: new_value".
 * Returns 0 on success. */
static int update_fm_field_on_disk(const char *filepath, const char *field, const char *new_value) {
    FILE *fp = fopen(filepath, "r");
    if (!fp) return -1;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *buf = malloc(sz + 1);
    if (!buf) { fclose(fp); return -1; }
    size_t nread = fread(buf, 1, sz, fp);
    buf[nread] = '\0';
    fclose(fp);

    /* Find field line */
    char needle[64];
    snprintf(needle, sizeof(needle), "\n%s:", field);
    char *hit = strstr(buf, needle);
    if (!hit) { free(buf); return -1; }
    char *line_start = hit + 1;  /* skip the \n */
    char *line_end = strchr(line_start, '\n');
    if (!line_end) line_end = buf + sz;

    /* Build new file content */
    char new_line[512];
    snprintf(new_line, sizeof(new_line), "%s: %s", field, new_value);

    size_t before_len = line_start - buf;
    size_t after_len = sz - (line_end - buf);
    size_t new_len = before_len + strlen(new_line) + after_len;
    char *out = malloc(new_len + 1);
    if (!out) { free(buf); return -1; }
    memcpy(out, buf, before_len);
    memcpy(out + before_len, new_line, strlen(new_line));
    memcpy(out + before_len + strlen(new_line), line_end, after_len);
    out[new_len] = '\0';

    fp = fopen(filepath, "w");
    if (!fp) { free(buf); free(out); return -1; }
    fwrite(out, 1, new_len, fp);
    fclose(fp);
    free(buf);
    free(out);
    return 0;
}

/* Accept a draft note: move to knowledge/<repo>/<topic>/, set status=verified.
 * Returns 1 on success. */
static int inbox_accept_note(KB_Entry *entry, const char *full_path,
                             const char *repo, const char *topic) {
    if (!repo || !repo[0] || !topic || !topic[0]) return 0;

    char *dest_dir = kb_path_join(g_tui.mdkb_root, repo);
    char *dest_dir2 = kb_path_join(dest_dir, topic);
    mkdir(dest_dir, 0755);
    mkdir(dest_dir2, 0755);

    const char *bname = strrchr(entry->path, '/');
    bname = bname ? bname + 1 : entry->path;
    char *dest_path = kb_path_join(dest_dir2, bname);

    update_fm_field_on_disk(full_path, "status", "verified");
    int ok = (rename(full_path, dest_path) == 0);

    if (ok) {
        /* Update in-memory path so collect_unique_topics() sees the new topic */
        char new_rel[4096];
        snprintf(new_rel, sizeof(new_rel), "%s/%s/%s", repo, topic, bname);
        free(entry->path);
        entry->path = kb_strdup(new_rel);
    }

    free(dest_dir);
    free(dest_dir2);
    free(dest_path);
    return ok;
}

/* Update tags array in YAML frontmatter on disk.
 * Writes tags: ["tag1", "tag2", ...] format.
 * Returns 0 on success. */
static int update_fm_tags_on_disk(const char *filepath, char **tags, size_t tag_count) {
    FILE *fp = fopen(filepath, "r");
    if (!fp) return -1;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *buf = malloc(sz + 1);
    if (!buf) { fclose(fp); return -1; }
    size_t nread = fread(buf, 1, sz, fp);
    buf[nread] = '\0';
    fclose(fp);

    /* Build new tags line */
    char new_line[1024];
    int pos = snprintf(new_line, sizeof(new_line), "tags: [");
    for (size_t i = 0; i < tag_count && pos < (int)sizeof(new_line) - 10; i++) {
        if (i > 0) pos += snprintf(new_line + pos, sizeof(new_line) - pos, ", ");
        pos += snprintf(new_line + pos, sizeof(new_line) - pos, "\"%s\"", tags[i]);
    }
    snprintf(new_line + pos, sizeof(new_line) - pos, "]");

    char *hit = strstr(buf, "\ntags:");
    if (!hit) {
        /* No tags field — insert before closing --- */
        char *fm_end = strstr(buf + 3, "---");
        if (!fm_end) { free(buf); return -1; }
        size_t before_len = fm_end - buf;
        size_t after_len = sz - before_len;
        size_t nl_len = strlen(new_line);
        char *out = malloc(before_len + nl_len + 1 + after_len + 1);
        if (!out) { free(buf); return -1; }
        memcpy(out, buf, before_len);
        memcpy(out + before_len, new_line, nl_len);
        out[before_len + nl_len] = '\n';
        memcpy(out + before_len + nl_len + 1, fm_end, after_len);
        out[before_len + nl_len + 1 + after_len] = '\0';
        fp = fopen(filepath, "w");
        if (fp) { fwrite(out, 1, before_len + nl_len + 1 + after_len, fp); fclose(fp); }
        free(out);
        free(buf);
        return 0;
    }

    char *line_start = hit + 1;
    char *line_end = strchr(line_start, '\n');
    if (!line_end) line_end = buf + sz;

    size_t before_len = line_start - buf;
    size_t after_len = sz - (line_end - buf);
    size_t nl_len = strlen(new_line);
    char *out = malloc(before_len + nl_len + after_len + 1);
    if (!out) { free(buf); return -1; }
    memcpy(out, buf, before_len);
    memcpy(out + before_len, new_line, nl_len);
    memcpy(out + before_len + nl_len, line_end, after_len);
    out[before_len + nl_len + after_len] = '\0';

    fp = fopen(filepath, "w");
    if (fp) { fwrite(out, 1, before_len + nl_len + after_len, fp); fclose(fp); }
    free(out);
    free(buf);
    return 0;
}

/* Inline title editor popup.
 * Pre-fills with current_title; user edits in place.
 * Returns new strdup'd string, or NULL if cancelled / unchanged. */
static char *do_title_edit_popup(const char *current_title) {
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);

    int popup_w = 64;
    if (popup_w > max_x - 4) popup_w = max_x - 4;
    int popup_h = 5;
    int start_y = (max_y - popup_h) / 2;
    int start_x = (max_x - popup_w) / 2;

    char buf[512] = "";
    int blen = 0;
    if (current_title) {
        strncpy(buf, current_title, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        blen = (int)strlen(buf);
    }

    curs_set(1);
    while (1) {
        /* Background */
        for (int y = start_y; y < start_y + popup_h; y++)
            mvhline(y, start_x, ' ', popup_w);

        /* Border */
        attron(A_BOLD | COLOR_PAIR(CP_HEADER));
        ui_hline(start_y, start_x, popup_w);
        ui_hline(start_y + popup_h - 1, start_x, popup_w);
        for (int y = start_y; y < start_y + popup_h; y++) {
            mvaddstr(y, start_x, "│");
            mvaddstr(y, start_x + popup_w - 1, "│");
        }
        mvaddstr(start_y, start_x, "┌");
        mvaddstr(start_y, start_x + popup_w - 1, "┐");
        mvaddstr(start_y + popup_h - 1, start_x, "└");
        mvaddstr(start_y + popup_h - 1, start_x + popup_w - 1, "┘");
        mvprintw(start_y + 1, start_x + 2, "Edit Title:");
        attroff(A_BOLD | COLOR_PAIR(CP_HEADER));

        /* Separator */
        attron(COLOR_PAIR(CP_HEADER));
        ui_hline(start_y + 2, start_x + 1, popup_w - 2);
        attroff(COLOR_PAIR(CP_HEADER));

        /* Input field */
        int field_x = start_x + 2;
        int field_w = popup_w - 4;
        /* Show last field_w chars if too long */
        int disp_start = blen > field_w ? blen - field_w : 0;
        mvhline(start_y + 3, field_x, ' ', field_w);
        mvprintw(start_y + 3, field_x, "%.*s", field_w, buf + disp_start);
        move(start_y + 3, field_x + (blen - disp_start));

        /* Help */
        attron(A_DIM);
        mvprintw(start_y + popup_h - 1, start_x + 2, " Enter:Confirm  Esc:Cancel ");
        attroff(A_DIM);

        refresh();
        int key = getch();
        if (key == '\n' || key == '\r') {
            curs_set(0);
            if (blen == 0 || (current_title && strcmp(buf, current_title) == 0))
                return NULL;
            return kb_strdup(buf);
        } else if (key == 27 || key == CTRL('c')) {
            curs_set(0);
            return NULL;
        } else if (key == KEY_BACKSPACE || key == 127 || key == 8) {
            if (blen > 0) buf[--blen] = '\0';
        } else if (key == CTRL('u')) {
            buf[0] = '\0';
            blen = 0;
        } else if (key >= 32 && key < 127 && blen < (int)sizeof(buf) - 1) {
            buf[blen++] = (char)key;
            buf[blen] = '\0';
        }
    }
}

/* Tag editing popup for inbox review.
 * Shows current tags with option to add/remove.
 * Returns 1 if tags were modified, 0 otherwise. */
static int do_tag_edit_popup(KB_Entry *entry, const char *filepath) {
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);

    /* Working copy of tags */
    size_t cap = entry->tag_count + 16;
    char **tags = kb_malloc(cap * sizeof(char *));
    size_t count = entry->tag_count;
    for (size_t i = 0; i < count; i++)
        tags[i] = kb_strdup(entry->tags[i]);

    int sel = 0;
    int modified = 0;

    while (1) {
        int popup_w = 40;
        if (popup_w > max_x - 4) popup_w = max_x - 4;
        int popup_h = (int)count + 8;
        if (popup_h > max_y - 4) popup_h = max_y - 4;
        if (popup_h < 8) popup_h = 8;
        int visible = popup_h - 7;
        int start_y = (max_y - popup_h) / 2;
        int start_x = (max_x - popup_w) / 2;

        /* Draw popup background */
        for (int y = start_y; y < start_y + popup_h; y++)
            mvhline(y, start_x, ' ', popup_w);

        /* Border */
        attron(A_BOLD | COLOR_PAIR(CP_HEADER));
        ui_hline(start_y, start_x, popup_w);
        ui_hline(start_y + popup_h - 1, start_x, popup_w);
        for (int y = start_y; y < start_y + popup_h; y++) {
            mvaddstr(y, start_x, "│");
            mvaddstr(y, start_x + popup_w - 1, "│");
        }
        mvaddstr(start_y, start_x, "┌");
        mvaddstr(start_y, start_x + popup_w - 1, "┐");
        mvaddstr(start_y + popup_h - 1, start_x, "└");
        mvaddstr(start_y + popup_h - 1, start_x + popup_w - 1, "┘");

        /* Title */
        mvprintw(start_y + 1, start_x + 2, "Edit Tags:");
        attroff(A_BOLD | COLOR_PAIR(CP_HEADER));

        /* Separator */
        attron(COLOR_PAIR(CP_HEADER));
        ui_hline(start_y + 2, start_x + 1, popup_w - 2);
        attroff(COLOR_PAIR(CP_HEADER));

        /* Tag list */
        if (count == 0) {
            attron(A_DIM);
            mvprintw(start_y + 3, start_x + 4, "(no tags)");
            attroff(A_DIM);
        } else {
            for (int i = 0; i < visible && (size_t)i < count; i++) {
                int y = start_y + 3 + i;
                if (i == sel) {
                    attron(COLOR_PAIR(CP_HIGHLIGHT));
                    mvhline(y, start_x + 1, ' ', popup_w - 2);
                }
                mvprintw(y, start_x + 2, "%s%.*s",
                         i == sel ? "> " : "  ",
                         popup_w - 6, tags[i]);
                if (i == sel)
                    attroff(COLOR_PAIR(CP_HIGHLIGHT));
            }
        }

        /* Help bar */
        attron(A_DIM);
        ui_hline(start_y + popup_h - 3, start_x + 1, popup_w - 2);
        mvprintw(start_y + popup_h - 2, start_x + 2, "a:Add  d:Del  Enter:Done  Esc:Cancel");
        attroff(A_DIM);

        refresh();

        int key = getch();
        switch (key) {
        case 'j': case KEY_DOWN:
            if (count > 0 && sel < (int)count - 1) sel++;
            break;
        case 'k': case KEY_UP:
            if (sel > 0) sel--;
            break;
        case 'd': case 'x':
            /* Delete selected tag */
            if (count > 0 && sel < (int)count) {
                free(tags[sel]);
                for (size_t i = sel; i + 1 < count; i++)
                    tags[i] = tags[i + 1];
                count--;
                if (sel > 0 && sel >= (int)count) sel--;
                modified = 1;
            }
            break;
        case 'a': {
            /* Add tag via selection popup (existing tags + new) */
            char *chosen = do_tag_select_popup();
            if (chosen) {
                /* Check for duplicate before adding */
                int dup = 0;
                for (size_t i = 0; i < count; i++) {
                    if (strcmp(tags[i], chosen) == 0) { dup = 1; break; }
                }
                if (!dup) {
                    if (count >= cap) {
                        cap *= 2;
                        tags = realloc(tags, cap * sizeof(char *));
                    }
                    tags[count++] = chosen;  /* take ownership */
                    modified = 1;
                } else {
                    free(chosen);
                }
            }
            break;
        }
        case '\n': case '\r':
            /* Done — save if modified */
            if (modified) {
                update_fm_tags_on_disk(filepath, tags, count);
                /* Update in-memory entry tags */
                for (size_t i = 0; i < entry->tag_count; i++) free(entry->tags[i]);
                free(entry->tags);
                entry->tags = kb_malloc(count * sizeof(char *));
                entry->tag_count = count;
                for (size_t i = 0; i < count; i++)
                    entry->tags[i] = kb_strdup(tags[i]);
            }
            goto tag_done;
        case 27: case CTRL('c'):
            modified = 0;  /* cancel — don't save */
            goto tag_done;
        }
    }

tag_done:
    for (size_t i = 0; i < count; i++) free(tags[i]);
    free(tags);
    return modified;
}

/* Run inbox review mode with dual-pane layout and markdown preview. */
static void do_inbox_review(void) {
    /* Collect draft entries */
    size_t draft_count = 0;
    size_t *draft_indices = NULL;
    size_t draft_cap = 0;

    for (size_t i = 0; i < g_tui.index->entry_count; i++) {
        KB_Entry *e = &g_tui.index->entries[i];
        if (e->status && strcmp(e->status, "draft") == 0) {
            if (draft_count >= draft_cap) {
                draft_cap = draft_cap ? draft_cap * 2 : 16;
                draft_indices = realloc(draft_indices, draft_cap * sizeof(size_t));
            }
            draft_indices[draft_count++] = i;
        }
    }

    if (draft_count == 0) {
        int max_y, max_x;
        getmaxyx(stdscr, max_y, max_x);
        (void)max_x;
        mvprintw(max_y - 1, 1, "No draft notes in inbox. Press any key.");
        refresh();
        getch();
        free(draft_indices);
        return;
    }

    int cur = 0;
    int preview_scroll = 0;
    int changed = 0;

    while (cur >= 0 && cur < (int)draft_count) {
        KB_Entry *entry = &g_tui.index->entries[draft_indices[cur]];

        char *full_path = kb_path_join(g_tui.mdkb_root, entry->path);
        struct stat fst;
        if (stat(full_path, &fst) != 0) {
            free(full_path);
            cur++;
            continue;
        }

        char *fm_repo = extract_fm_field(entry->raw_content, "repo");
        char *fm_topic = extract_fm_field(entry->raw_content, "topic");
        char *fm_type = extract_fm_field(entry->raw_content, "type");

        /* Split content into lines for preview rendering */
        size_t line_count = 0;
        size_t line_cap = 256;
        char **lines = malloc(line_cap * sizeof(char *));
        if (entry->content) {
            const char *p = entry->content;
            while (*p) {
                const char *nl = strchr(p, '\n');
                size_t len = nl ? (size_t)(nl - p) : strlen(p);
                if (line_count >= line_cap) {
                    line_cap *= 2;
                    lines = realloc(lines, line_cap * sizeof(char *));
                }
                lines[line_count++] = kb_strndup(p, len);
                p = nl ? nl + 1 : p + len;
            }
        }

        /* Clamp preview_scroll */
        if (preview_scroll < 0) preview_scroll = 0;

        int redraw = 1;
        int content_h = 0;

        while (1) {
            if (redraw) {
                int max_y, max_x;
                getmaxyx(stdscr, max_y, max_x);
                content_h = max_y - 4;  /* header row + padding - action bar */
                clear();

                /* Header bar */
                attron(A_BOLD | COLOR_PAIR(CP_HEADER));
                mvhline(0, 0, ' ', max_x);
                mvprintw(0, 1, " INBOX REVIEW (%d/%d)", cur + 1, (int)draft_count);
                attroff(A_BOLD | COLOR_PAIR(CP_HEADER));

                /* Layout: left pane = metadata, right pane = preview */
                int left_w = 30;
                if (left_w > max_x / 3) left_w = max_x / 3;
                if (left_w < 20) left_w = 20;
                int right_x = left_w + 1;
                int right_w = max_x - right_x - 1;
                int content_y = 2;
                content_h = max_y - content_y - 2;  /* leave room for action bar */

                /* === Draw right pane FIRST (may overflow via cursor wrap) === */

                /* Right pane: markdown preview with render_markdown_line */
                int max_scroll = (int)line_count > content_h ? (int)line_count - content_h : 0;
                if (preview_scroll > max_scroll)
                    preview_scroll = max_scroll;
                if (preview_scroll < 0) preview_scroll = 0;

                /* Pre-compute in_code state for lines before scroll offset */
                int in_code = 0;
                for (int li = 0; li < preview_scroll && (size_t)li < line_count; li++) {
                    if (check_code_fence(lines[li]))
                        in_code = !in_code;
                }

                for (int row = 0; row < content_h; row++) {
                    int li = preview_scroll + row;
                    if ((size_t)li >= line_count) break;
                    int sy = content_y + row;
                    in_code = render_markdown_line(lines[li], sy, right_x + 1, right_w - 2, in_code);
                }

                /* Scroll indicator */
                if (line_count > (size_t)content_h) {
                    attron(A_DIM);
                    mvprintw(max_y - 3, right_x + 1, "-- %d/%d --",
                             preview_scroll + 1, (int)line_count);
                    attroff(A_DIM);
                }

                /* === Now draw left pane OVER any cursor-wrap overflow === */

                /* Clear left pane area + separator to cover wrap artifacts */
                for (int sy = content_y; sy < max_y - 2; sy++) {
                    mvhline(sy, 0, ' ', left_w);
                    mvaddstr(sy, left_w, "│");
                }

                /* Left pane: metadata (all clamped to left_w - 3) */
                int lw_max = left_w - 3;

                attron(A_BOLD | COLOR_PAIR(CP_HEADER));
                mvprintw(content_y, 2, "Title");
                attroff(A_BOLD | COLOR_PAIR(CP_HEADER));
                mvprintw(content_y + 1, 2, " %.*s", lw_max,
                         entry->title ? entry->title : "Untitled");

                attron(A_BOLD | COLOR_PAIR(CP_HEADER));
                mvprintw(content_y + 3, 2, "Repo");
                attroff(A_BOLD | COLOR_PAIR(CP_HEADER));
                attron(COLOR_PAIR(CP_TAG));
                mvprintw(content_y + 4, 2, " %.*s", lw_max,
                         fm_repo ? fm_repo : "(unknown)");
                attroff(COLOR_PAIR(CP_TAG));

                attron(A_BOLD | COLOR_PAIR(CP_HEADER));
                mvprintw(content_y + 6, 2, "Topic");
                attroff(A_BOLD | COLOR_PAIR(CP_HEADER));
                attron(COLOR_PAIR(CP_TAG));
                mvprintw(content_y + 7, 2, " %.*s", lw_max,
                         fm_topic ? fm_topic : "(unknown)");
                attroff(COLOR_PAIR(CP_TAG));

                attron(A_BOLD | COLOR_PAIR(CP_HEADER));
                mvprintw(content_y + 9, 2, "Type");
                attroff(A_BOLD | COLOR_PAIR(CP_HEADER));
                attron(COLOR_PAIR(CP_CODE));
                mvprintw(content_y + 10, 2, " %.*s", lw_max,
                         fm_type ? fm_type : "(unknown)");
                attroff(COLOR_PAIR(CP_CODE));

                /* Tags */
                if (entry->tag_count > 0) {
                    attron(A_BOLD | COLOR_PAIR(CP_HEADER));
                    mvprintw(content_y + 12, 2, "Tags");
                    attroff(A_BOLD | COLOR_PAIR(CP_HEADER));
                    for (size_t ti = 0; ti < entry->tag_count && (int)(content_y + 13 + ti) < max_y - 3; ti++) {
                        attron(A_DIM);
                        mvprintw(content_y + 13 + (int)ti, 2, " %.*s",
                                 lw_max, entry->tags[ti]);
                        attroff(A_DIM);
                    }
                }

                /* Action bar */
                attron(A_REVERSE);
                mvhline(max_y - 1, 0, ' ', max_x);
                mvprintw(max_y - 1, 1,
                         " a:Accept  e:Edit  t:Tags  d:Discard  s/J:Next  K:Prev  q:Quit ");
                attroff(A_REVERSE);

                refresh();
                redraw = 0;
            }

            int ch = getch();
            int max_scroll = (int)line_count > content_h ? (int)line_count - content_h : 0;
            switch (ch) {
            case 'j': case KEY_DOWN:
                /* Scroll preview down */
                if (preview_scroll < max_scroll) {
                    preview_scroll++;
                    redraw = 1;
                }
                break;
            case 'k': case KEY_UP:
                /* Scroll preview up */
                if (preview_scroll > 0) {
                    preview_scroll--;
                    redraw = 1;
                }
                break;
            case CTRL('d'):
                /* Half page down */
                preview_scroll += content_h / 2;
                if (preview_scroll > max_scroll) preview_scroll = max_scroll;
                redraw = 1;
                break;
            case CTRL('u'):
                /* Half page up */
                preview_scroll -= content_h / 2;
                if (preview_scroll < 0) preview_scroll = 0;
                redraw = 1;
                break;
            case CTRL('f'): case KEY_NPAGE: case ' ':
                /* Full page down */
                preview_scroll += content_h;
                if (preview_scroll > max_scroll) preview_scroll = max_scroll;
                redraw = 1;
                break;
            case CTRL('b'): case KEY_PPAGE:
                /* Full page up */
                preview_scroll -= content_h;
                if (preview_scroll < 0) preview_scroll = 0;
                redraw = 1;
                break;
            case 'G':
                /* Go to bottom */
                preview_scroll = max_scroll;
                redraw = 1;
                break;
            case 'g':
                /* gg = go to top */
                {
                    int next = getch();
                    if (next == 'g') {
                        preview_scroll = 0;
                        redraw = 1;
                    }
                }
                break;
            case 'J': case 's':
                /* Next note */
                cur++;
                preview_scroll = 0;
                goto next_note;
            case 'K':
                /* Previous note */
                if (cur > 0) {
                    cur--;
                    preview_scroll = 0;
                    goto next_note;
                }
                break;
            case 'a': {
                /* Accept with current repo/topic */
                if (!fm_repo || !fm_topic) {
                    int my, mx;
                    getmaxyx(stdscr, my, mx);
                    (void)mx;
                    mvprintw(my - 2, 1, "Cannot accept: missing repo or topic. Press any key.");
                    refresh();
                    getch();
                    redraw = 1;
                    break;
                }
                if (inbox_accept_note(entry, full_path, fm_repo, fm_topic))
                    changed = 1;
                cur++;
                preview_scroll = 0;
                goto next_note;
            }
            case 'e': {
                /* Edit: pick topic via popup, then type via popup, stay on note */
                /* Step 1: Topic popup */
                char *new_topic = do_topic_filter_popup();
                if (!new_topic) { redraw = 1; break; }  /* cancelled */

                /* Step 2: Type popup */
                char *new_type = do_type_filter_popup();
                if (!new_type) {
                    free(new_topic);
                    redraw = 1;
                    break;
                }

                /* Update fields on disk */
                update_fm_field_on_disk(full_path, "topic", new_topic);
                update_fm_field_on_disk(full_path, "type", new_type);

                /* Update local display values */
                free(fm_topic);
                fm_topic = new_topic;  /* take ownership */
                free(fm_type);
                fm_type = new_type;    /* take ownership */

                redraw = 1;
                break;
            }
            case 't': {
                /* Edit tags */
                if (do_tag_edit_popup(entry, full_path))
                    changed = 1;
                redraw = 1;
                break;
            }
            case 'd':
                /* Discard */
                unlink(full_path);
                changed = 1;
                cur++;
                preview_scroll = 0;
                goto next_note;
            case 'q': case 27:
                /* Free lines and exit */
                for (size_t li = 0; li < line_count; li++) free(lines[li]);
                free(lines);
                free(fm_repo);
                free(fm_topic);
                free(fm_type);
                free(full_path);
                goto done;
            default:
                break;
            }
        }

next_note:
        for (size_t li = 0; li < line_count; li++) free(lines[li]);
        free(lines);
        free(fm_repo);
        free(fm_topic);
        free(fm_type);
        free(full_path);
    }

done:
    free(draft_indices);

    if (changed) {
        tui_reload_current_index();
    }
}

/* Refresh display */
static void refresh_display(void) {
    clear();
    draw_header();
    draw_left_pane();
    draw_right_pane();
    draw_status_bar();
    refresh();
}

/* Handle search input */
static void do_search(void) {
    /* Enable cursor for input */
    curs_set(1);
    echo();

    int max_y, unused_x;
    getmaxyx(stdscr, max_y, unused_x);
    (void)unused_x;

    char query[256] = {0};
    mvprintw(max_y - 1, 1, "Search: ");
    getnstr(query, sizeof(query) - 1);

    noecho();
    curs_set(0);

    if (strlen(query) > 0) {
        /* Free old search */
        if (g_tui.search_query) {
            free(g_tui.search_query);
        }
        if (g_tui.results) {
            mdmdkb_search_free(g_tui.results);
        }

        /* New search */
        g_tui.search_query = kb_strdup(query);
        g_tui.results = mdkb_search(g_tui.index, query);
        if (g_tui.archive_mode && g_tui.results && g_tui.results->count > 1)
            qsort(g_tui.results->results, g_tui.results->count,
                  sizeof(SearchResult), cmp_search_result_by_time_desc);
        g_tui.selected = 0;
        g_tui.offset = 0;
        g_tui.top_offset = 0;
        invalidate_filter_cache();

        /* Auto-jump to first match in the previewed entry */
        KB_Entry *pe = get_preview_entry();
        if (pe && pe->raw_content) {
            build_match_cache(pe->id, pe->raw_content, query);
            int found = find_match_line(pe->raw_content, query, -1, +1);
            g_tui.preview_match_line = found;
            {   int _my2, _mx2;
                            getmaxyx(stdscr, _my2, _mx2);
                            (void)_mx2;
                            int _h = (_my2 - 3) / 2;
                            g_tui.top_offset = found > _h ? found - _h : 0;
                        }
        } else {
            g_tui.preview_match_line = -1;
        }
    }
}

/* Helper: fork+exec a child in its own process group, wait for exit or stop.
 * Using a separate process group ensures kill(-pid) stops the entire child tree
 * (important when child is sh -c which spawns claude as a grandchild).
 * Returns: 0 = exited normally, 1 = child suspended, -1 = error */
static int run_child(const char *const argv[], const char *cwd) {
    /* Ignore SIGTTOU so parent can call tcsetpgrp from background */
    struct sigaction sa_ign, sa_old_ttou;
    memset(&sa_ign, 0, sizeof(sa_ign));
    sa_ign.sa_handler = SIG_IGN;
    sigaction(SIGTTOU, &sa_ign, &sa_old_ttou);

    pid_t pid = fork();
    if (pid < 0) {
        sigaction(SIGTTOU, &sa_old_ttou, NULL);
        return -1;
    }

    if (pid == 0) {
        /* Child: own process group + terminal foreground */
        setpgid(0, 0);
        tcsetpgrp(STDIN_FILENO, getpgrp());
        signal(SIGTTOU, SIG_DFL);
        signal(SIGTTIN, SIG_DFL);
        if (cwd && cwd[0]) chdir(cwd);
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }

    /* Parent: put child in its own group, give it the terminal */
    setpgid(pid, pid);
    tcsetpgrp(STDIN_FILENO, pid);

    g_claude_pid = pid;
    int status;
    waitpid(pid, &status, WUNTRACED);

    /* Take back terminal foreground */
    tcsetpgrp(STDIN_FILENO, getpgrp());

    if (WIFSTOPPED(status)) {
        /* Child stopped (Ctrl+Z). Re-stop entire child process group
         * so mdkb TUI can take over. */
        kill(-pid, SIGSTOP);
        sigaction(SIGTTOU, &sa_old_ttou, NULL);
        return 1;
    }
    if (WIFEXITED(status)) {
        g_claude_pid = 0;
        sigaction(SIGTTOU, &sa_old_ttou, NULL);
        return WEXITSTATUS(status) == 0 ? 0 : -1;
    }
    g_claude_pid = 0;
    sigaction(SIGTTOU, &sa_old_ttou, NULL);
    return -1;
}

/* Find the original working directory for a session by reading its JSONL file.
 * Searches ~/.claude/projects/ for SESSION_ID.jsonl and extracts the "cwd" field
 * from the first user message. Returns a malloc'd string or NULL. */
static char *find_session_cwd(const char *session_id) {
    const char *home = getenv("HOME");
    if (!home || !session_id || !session_id[0]) return NULL;

    char projects_dir[PATH_MAX];
    snprintf(projects_dir, sizeof(projects_dir), "%s/.claude/projects", home);

    DIR *d = opendir(projects_dir);
    if (!d) return NULL;

    char *result = NULL;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;

        char jsonl_path[PATH_MAX];
        snprintf(jsonl_path, sizeof(jsonl_path), "%s/%s/%s.jsonl",
                 projects_dir, ent->d_name, session_id);

        FILE *fp = fopen(jsonl_path, "r");
        if (!fp) continue;

        /* Scan lines for first occurrence of "cwd":"..." */
        char line[4096];
        while (fgets(line, sizeof(line), fp)) {
            char *p = strstr(line, "\"cwd\":\"");
            if (!p) continue;
            p += 7; /* skip "cwd":"  */
            char *end = strchr(p, '"');
            if (!end) continue;
            result = kb_strndup(p, (size_t)(end - p));
            break;
        }
        fclose(fp);
        if (result) break;
    }
    closedir(d);
    return result;
}

/* Launch Claude Code as a child process.
 * If session_id is non-NULL, resumes that session (falls back to context load on failure).
 * If context_path is non-NULL, used for fallback context loading.
 * If cwd is non-NULL, changes to that directory first.
 * Supports Ctrl+Z: child stops → TUI resumes, press 'R' to continue. */
static void launch_claude(const char *session_id, const char *cwd) {
    /* Save session_id for statusbar display */
    if (session_id && session_id[0])
        snprintf(g_claude_session_id, sizeof(g_claude_session_id), "%s", session_id);
    else
        g_claude_session_id[0] = '\0';

    /* Find the selected entry's file path for context loading */
    char *context_path = NULL;
    if (g_tui.mdkb_root) {
        /* Get selected entry's full path */
        KB_Entry *sel = NULL;
        size_t cnt = effective_count();
        if (g_tui.selected >= 0 && (size_t)g_tui.selected < cnt) {
            sel = effective_entry_at(g_tui.selected);
        }
        if (sel && sel->path)
            context_path = kb_path_join(g_tui.mdkb_root, sel->path);
    }

    mdkb_tui_cleanup();

    /* Step 1: try claude --resume */
    if (session_id && session_id[0]) {
        const char *argv[] = {"claude", "--model", "sonnet", "--permission-mode", "auto", "--resume", session_id, NULL};
        int rc = run_child(argv, cwd);
        if (rc == 1) {
            /* Suspended — restore TUI (no rescan needed, index is still valid) */
            free(context_path);
            restore_after_claude(0);
            return;
        }
        if (rc == 0) {
            /* Normal exit */
            free(context_path);
            restore_after_claude(1);
            return;
        }
        /* rc == -1: resume failed — fall through to context load */
    }

    /* Step 2: resume failed — start claude with note as system-level guidance.
     * Uses --append-system-prompt-file to let claude read the file directly,
     * keeping built-in capabilities while injecting note as background knowledge. */
    if (context_path) {
        const char *argv[] = {"claude", "--model", "sonnet", "--permission-mode", "auto",
                              "--append-system-prompt-file", context_path, NULL};
        int rc2 = run_child(argv, cwd);
        free(context_path);
        restore_after_claude(rc2 != 1);
        return;
    }

    /* Step 3: no context — just start blank claude */
    {
        const char *argv[] = {"claude", "--model", "sonnet", "--permission-mode", "auto", NULL};
        int rc3 = run_child(argv, cwd);
        restore_after_claude(rc3 != 1);
    }
}

/* Check if any loaded marks were removed (unmarked) */
static bool marks_have_removals(void) {
    if (!g_tui.loaded_marks || g_tui.loaded_mark_count == 0) return false;
    for (size_t i = 0; i < g_tui.index->entry_capacity; i++) {
        if (g_tui.loaded_marks[i] && !g_tui.marked[i]) return true;
    }
    return false;
}

/* Check if current marks are exactly the same as loaded marks */
static bool marks_unchanged(void) {
    if (!g_tui.loaded_marks) return g_tui.mark_count == 0;
    if (g_tui.mark_count != g_tui.loaded_mark_count) return false;
    return memcmp(g_tui.marked, g_tui.loaded_marks,
                  g_tui.index->entry_capacity * sizeof(bool)) == 0;
}

/* Snapshot current marks into loaded_marks */
static void snapshot_loaded_marks(void) {
    if (g_tui.loaded_marks && g_tui.marked) {
        memcpy(g_tui.loaded_marks, g_tui.marked,
               g_tui.index->entry_capacity * sizeof(bool));
    }
    g_tui.loaded_mark_count = g_tui.mark_count;
}

/* Clear loaded marks (when session ends) */
static void clear_loaded_marks(void) {
    if (g_tui.loaded_marks) {
        memset(g_tui.loaded_marks, 0,
               g_tui.index->entry_capacity * sizeof(bool));
    }
    g_tui.loaded_mark_count = 0;
}

/* Collect paths of marked entries into a dynamically allocated string array.
 * Caller must free each string and the array itself. */
static char **collect_mark_paths(const bool *marks, size_t count, size_t *out_count) {
    *out_count = 0;
    if (!marks || count == 0) return NULL;

    /* marked[] is indexed by entry ID, not array position.
     * Walk entries and check marks[entry.id]. */
    size_t n = 0;
    for (size_t i = 0; i < g_tui.index->entry_count; i++) {
        uint64_t id = g_tui.index->entries[i].id;
        if (id < g_tui.index->entry_capacity && marks[id]) n++;
    }
    if (n == 0) return NULL;

    char **paths = malloc(n * sizeof(char *));
    for (size_t i = 0; i < g_tui.index->entry_count; i++) {
        uint64_t id = g_tui.index->entries[i].id;
        if (id < g_tui.index->entry_capacity && marks[id] && g_tui.index->entries[i].path)
            paths[(*out_count)++] = kb_strdup(g_tui.index->entries[i].path);
    }
    return paths;
}

/* Free a path array returned by collect_mark_paths */
static void free_mark_paths(char **paths, size_t count) {
    if (!paths) return;
    for (size_t i = 0; i < count; i++)
        free(paths[i]);
    free(paths);
}

/* Restore marks from a path array by looking up entries in the current index */
static size_t restore_marks_from_paths(bool *marks, char **paths, size_t path_count) {
    if (!marks || !paths || path_count == 0) return 0;
    size_t restored = 0;
    for (size_t i = 0; i < path_count; i++) {
        for (size_t j = 0; j < g_tui.index->entry_count; j++) {
            if (g_tui.index->entries[j].path &&
                strcmp(g_tui.index->entries[j].path, paths[i]) == 0) {
                uint64_t id = g_tui.index->entries[j].id;
                if (id < g_tui.index->entry_capacity) {
                    marks[id] = true;
                    restored++;
                }
                break;
            }
        }
    }
    return restored;
}

/* Kill a stopped claude process gracefully.
 * Must give terminal foreground to claude before SIGCONT, otherwise it gets
 * SIGTTIN when trying to read stdin and stops again, causing waitpid to hang. */
static void kill_stopped_claude(void) {
    if (g_claude_pid <= 0) return;

    signal(SIGTTOU, SIG_IGN);
    tcsetpgrp(STDIN_FILENO, g_claude_pid);

    kill(-g_claude_pid, SIGCONT);
    kill(-g_claude_pid, SIGTERM);

    /* Wait with timeout — SIGKILL if claude doesn't exit within 3 seconds */
    int status;
    for (int i = 0; i < 30; i++) {
        pid_t ret = waitpid(g_claude_pid, &status, WNOHANG);
        if (ret != 0) goto done;
        struct timespec ts = {0, 100000000};  /* 100ms */
        nanosleep(&ts, NULL);
    }
    kill(-g_claude_pid, SIGKILL);
    waitpid(g_claude_pid, &status, 0);

done:
    tcsetpgrp(STDIN_FILENO, getpgrp());
    signal(SIGTTOU, SIG_DFL);

    g_claude_pid = 0;
    g_claude_session_id[0] = '\0';
}

/* Build the --append-system-prompt text listing all marked files, grouped by topic */
static char *build_marked_files_prompt(bool mark_new) {
    typedef struct { KB_Entry *e; char topic[128]; } MarkedItem;

    size_t marked_total = 0;
    size_t buf_size = 1536;
    for (size_t i = 0; i < g_tui.index->entry_count; i++) {
        KB_Entry *e = &g_tui.index->entries[i];
        if (!g_tui.marked[e->id]) continue;
        buf_size += strlen(e->path ? e->path : "") + strlen(e->title ? e->title : "") + 96;
        marked_total++;
    }
    buf_size += marked_total * 80; /* extra for topic section headers */

    char *prompt = malloc(buf_size);
    if (!prompt) return NULL;

    int pos = snprintf(prompt, buf_size,
        "# Knowledge Notes (mdkb)\n"
        "\n"
        "The user has loaded personal knowledge notes from their mdkb knowledge base.\n"
        "Each entry below shows a file path and title — the full content is NOT included here.\n"
        "\n"
        "## How to use\n"
        "- When the user asks about a topic covered by these notes, use the Read tool to read the relevant files BEFORE answering.\n"
        "- Match by title keywords to decide which files are relevant. You do not need to read all files.\n"
        "- If the user's question clearly relates to a loaded note, proactively read it — do not ask whether to look it up.\n"
        "- These notes may contain code snippets, architecture decisions, bug analyses, and implementation details from past work sessions.\n"
        "\n"
        "## [code] notes\n"
        "Entries marked `[code]` contain references to actual source code locations (absolute file paths and line numbers).\n"
        "**IMPORTANT: When the user asks to find, locate, or understand code, check [code] notes FIRST before using grep or search.**\n"
        "If a loaded [code] note's title matches the user's query, read it — it likely already has the exact file paths and\n"
        "line numbers you need, saving a grep.\n"
        "After reading a [code] note, go directly to the source locations mentioned to verify the current state — the note\n"
        "records what was true when written, always cross-check against the live codebase.\n"
        "\n"
        "## Fallback search\n"
        "If the loaded index below does not contain notes relevant to the user's question,\n"
        "search within the loaded topic directories (shown in each ### Topic header):\n"
        "  mdkb -q \"<keywords>\" -l 5 -p <topic-path>\n"
        "This returns JSON with path, title, and BM25 score. Read the top-scoring file(s) before answering.\n"
        "Use concise keywords from the user's question. Chinese queries are supported.\n"
        "Only read results with score >= 5.0 — lower scores are substring fallback noise, not genuine matches.\n");

    if (mark_new) {
        pos += snprintf(prompt + pos, buf_size - pos,
            "\n## [new] notes\n"
            "Notes marked **[NEW]** were added in this resume — be aware they exist "
            "and read them when the conversation touches related topics.\n");
    }
    pos += snprintf(prompt + pos, buf_size - pos, "\n## Loaded files\n");

    if (marked_total == 0) return prompt;

    /* Collect marked entries */
    MarkedItem *items = malloc(marked_total * sizeof(MarkedItem));
    if (!items) { free(prompt); return NULL; }

    size_t n = 0;
    for (size_t i = 0; i < g_tui.index->entry_count; i++) {
        KB_Entry *e = &g_tui.index->entries[i];
        if (!g_tui.marked[e->id]) continue;
        items[n].e = e;
        get_topic_from_path(e->path, items[n].topic, sizeof(items[n].topic));
        n++;
    }

    /* Sort by topic (insertion sort) */
    for (size_t i = 1; i < n; i++) {
        MarkedItem tmp = items[i];
        size_t j = i;
        while (j > 0 && strcmp(items[j-1].topic, tmp.topic) > 0) {
            items[j] = items[j-1];
            j--;
        }
        items[j] = tmp;
    }

    /* Emit grouped by topic */
    int num = 0;
    char cur_topic[128] = "";
    for (size_t i = 0; i < n; i++) {
        KB_Entry *e = items[i].e;
        const char *tp = items[i].topic;

        if (strcmp(tp, cur_topic) != 0) {
            /* Count entries in this topic group */
            size_t cnt = 0;
            for (size_t j = i; j < n && strcmp(items[j].topic, tp) == 0; j++) cnt++;

            if (tp[0]) {
                /* Derive topic directory from first entry path: "repo/topic/file.md" → "<root>/repo/topic" */
                char rel_dir[512];
                strncpy(rel_dir, e->path, sizeof(rel_dir) - 1);
                rel_dir[sizeof(rel_dir) - 1] = '\0';
                char *last_slash = strrchr(rel_dir, '/');
                if (last_slash) *last_slash = '\0';
                char *topic_dir = kb_path_join(g_tui.mdkb_root, rel_dir);
                pos += snprintf(prompt + pos, buf_size - pos,
                    "\n### Topic: %s (%zu note%s) — search path: %s\n",
                    tp, cnt, cnt == 1 ? "" : "s",
                    topic_dir ? topic_dir : rel_dir);
                free(topic_dir);
            } else {
                pos += snprintf(prompt + pos, buf_size - pos, "\n### Other notes\n");
            }
            strncpy(cur_topic, tp, sizeof(cur_topic) - 1);
            cur_topic[sizeof(cur_topic) - 1] = '\0';
        }

        char *full_path = kb_path_join(g_tui.mdkb_root, e->path);
        int is_code = 0;
        for (size_t ti = 0; ti < e->tag_count; ti++) {
            if (e->tags[ti] && strcmp(e->tags[ti], "type:code") == 0) { is_code = 1; break; }
        }
        int is_new = mark_new && g_tui.loaded_marks && !g_tui.loaded_marks[e->id];
        pos += snprintf(prompt + pos, buf_size - pos,
            "%d. %s — %s%s%s\n", ++num,
            full_path ? full_path : e->path,
            e->title ? e->title : "Untitled",
            is_code ? " [code]" : "",
            is_new  ? " [NEW]"  : "");
        free(full_path);
    }

    free(items);
    return prompt;
}

/* Launch Claude Code with multiple knowledge files as on-demand references.
 * Builds a prompt listing file paths + titles, passed via --append-system-prompt.
 * Pre-generates a session UUID so launch_claude_continue() can --resume it later. */
static void launch_claude_multi(void) {
    char *prompt = build_marked_files_prompt(false);
    if (!prompt) return;

    /* Generate UUID so we can resume this exact session later */
    FILE *fuuid = fopen("/proc/sys/kernel/random/uuid", "r");
    if (fuuid) {
        if (fgets(g_claude_session_id, sizeof(g_claude_session_id), fuuid))
            g_claude_session_id[strcspn(g_claude_session_id, "\n")] = '\0';
        fclose(fuuid);
    }

    mdkb_tui_cleanup();

    const char *argv[] = {"claude", "--model", "sonnet", "--permission-mode", "auto",
                          "--session-id", g_claude_session_id,
                          "--append-system-prompt", prompt, NULL};
    int rc = run_child(argv, NULL);

    if (rc == 1) {
        /* Suspended — keep marks, snapshot as loaded */
        snapshot_loaded_marks();
    } else {
        /* Exited — clear everything */
        memset(g_tui.marked, 0, g_tui.index->entry_capacity * sizeof(bool));
        g_tui.mark_count = 0;
        clear_loaded_marks();
    }

    free(prompt);
    restore_after_claude(rc != 1);
}

/* Resume the session with updated file list after user added more marks.
 * Saves the session ID before killing the old process, then --resume it. */
static void launch_claude_continue(void) {
    char *prompt = build_marked_files_prompt(true);
    if (!prompt) return;

    /* Save session ID before kill_stopped_claude() clears it */
    char saved_session[64];
    snprintf(saved_session, sizeof(saved_session), "%s", g_claude_session_id);

    kill_stopped_claude();
    mdkb_tui_cleanup();

    const char *argv[] = {"claude", "--resume", saved_session,
                          "--model", "sonnet", "--permission-mode", "auto",
                          "--append-system-prompt", prompt, NULL};
    int rc = run_child(argv, NULL);

    if (rc == 1) {
        /* Restore session ID so next continue/resume can find it */
        snprintf(g_claude_session_id, sizeof(g_claude_session_id),
                 "%s", saved_session);
        snapshot_loaded_marks();
    } else {
        memset(g_tui.marked, 0, g_tui.index->entry_capacity * sizeof(bool));
        g_tui.mark_count = 0;
        clear_loaded_marks();
    }

    free(prompt);
    restore_after_claude(rc != 1);
}

/* Resume a previously suspended Claude Code session */
static void resume_claude(void) {
    if (g_claude_pid <= 0) return;

    mdkb_tui_cleanup();

    /* Give terminal to child, then resume */
    signal(SIGTTOU, SIG_IGN);
    tcsetpgrp(STDIN_FILENO, g_claude_pid);
    kill(-g_claude_pid, SIGCONT);

    int status;
    waitpid(g_claude_pid, &status, WUNTRACED);

    /* Take back terminal */
    tcsetpgrp(STDIN_FILENO, getpgrp());
    signal(SIGTTOU, SIG_DFL);

    if (WIFSTOPPED(status)) {
        /* Ctrl+Z again — re-stop entire child group, restore TUI */
        kill(-g_claude_pid, SIGSTOP);
        restore_after_claude(0);
        return;
    }
    if (WIFEXITED(status) || WIFSIGNALED(status)) {
        /* Claude exited — clear state, restore TUI */
        g_claude_pid = 0;
        g_claude_session_id[0] = '\0';
        clear_loaded_marks();
        restore_after_claude(1);
        return;
    }

    g_claude_pid = 0;
    g_claude_session_id[0] = '\0';
    clear_loaded_marks();
    restore_after_claude(1);
}

/* Handle key press */
static void handle_key(int key) {
    size_t count = effective_count();

    switch (key) {
        case 'q':
        case 'Q': {
            /* Confirm quit with a centered popup */
            int max_y_q, max_x_q;
            getmaxyx(stdscr, max_y_q, max_x_q);
            int pw = 30, ph = 5;
            int py = (max_y_q - ph) / 2;
            int px = (max_x_q - pw) / 2;

            /* Draw popup box */
            for (int row = 0; row < ph; row++) {
                move(py + row, px);
                if (row == 0 || row == ph - 1) {
                    addch(row == 0 ? ACS_ULCORNER : ACS_LLCORNER);
                    for (int c = 0; c < pw - 2; c++) addstr("─");
                    addch(row == 0 ? ACS_URCORNER : ACS_LRCORNER);
                } else {
                    addstr("│");
                    for (int c = 0; c < pw - 2; c++) addch(' ');
                    addstr("│");
                }
            }
            /* Popup text */
            attron(A_BOLD);
            mvprintw(py + 1, px + (pw - 14) / 2, "Quit mdkb ?");
            attroff(A_BOLD);
            mvprintw(py + 3, px + 2, " y = Yes    n/Esc = No");

            refresh();
            timeout(-1);  /* block until user responds */
            int qch = getch();
            timeout(500);  /* restore polling interval */
            if (qch == 'y' || qch == 'Y')
                g_tui.running = 0;
            /* Any other key: cancel, return to normal view */
            break;
        }

        case '\t': {
            /* Tab: toggle archive ↔ knowledge mode using cached indexes */
            if (g_tui.archive_mode) {
                /* Switching from archive → knowledge */
                /* Save current marks to archive slots */
                g_tui.archive_marked = g_tui.marked;
                g_tui.archive_mark_count = g_tui.mark_count;
                g_tui.archive_loaded_marks = g_tui.loaded_marks;
                g_tui.archive_loaded_mark_count = g_tui.loaded_mark_count;

                /* Lazy-load knowledge index on first switch */
                if (!g_tui.knowledge_index) {
                    g_tui.knowledge_index = load_index_for_root(g_tui.knowledge_root);
                    /* Allocate marks for knowledge mode */
                    g_tui.knowledge_marked = calloc(g_tui.knowledge_index->entry_capacity, sizeof(bool));
                    g_tui.knowledge_mark_count = 0;
                    g_tui.knowledge_loaded_marks = calloc(g_tui.knowledge_index->entry_capacity, sizeof(bool));
                    g_tui.knowledge_loaded_mark_count = 0;
                }

                /* Switch to knowledge */
                tui_switch_to_index(g_tui.knowledge_index, g_tui.knowledge_root);
                g_tui.marked = g_tui.knowledge_marked;
                g_tui.mark_count = g_tui.knowledge_mark_count;
                g_tui.loaded_marks = g_tui.knowledge_loaded_marks;
                g_tui.loaded_mark_count = g_tui.knowledge_loaded_mark_count;
            } else {
                /* Switching from knowledge → archive */
                /* Save current marks to knowledge slots */
                g_tui.knowledge_marked = g_tui.marked;
                g_tui.knowledge_mark_count = g_tui.mark_count;
                g_tui.knowledge_loaded_marks = g_tui.loaded_marks;
                g_tui.knowledge_loaded_mark_count = g_tui.loaded_mark_count;

                /* Lazy-load archive index on first switch (unlikely since archive is default) */
                if (!g_tui.archive_index) {
                    g_tui.archive_index = load_index_for_root(g_tui.archive_root);
                    g_tui.archive_marked = calloc(g_tui.archive_index->entry_capacity, sizeof(bool));
                    g_tui.archive_mark_count = 0;
                    g_tui.archive_loaded_marks = calloc(g_tui.archive_index->entry_capacity, sizeof(bool));
                    g_tui.archive_loaded_mark_count = 0;
                }

                /* Switch to archive */
                tui_switch_to_index(g_tui.archive_index, g_tui.archive_root);
                g_tui.marked = g_tui.archive_marked;
                g_tui.mark_count = g_tui.archive_mark_count;
                g_tui.loaded_marks = g_tui.archive_loaded_marks;
                g_tui.loaded_mark_count = g_tui.archive_loaded_mark_count;
            }
            g_tui.archive_mode = !g_tui.archive_mode;
            /* Re-setup inotify watches for the new directory */
            inotify_setup(g_tui.mdkb_root);
            break;
        }

        case CTRL('d'): {
            /* Half-page down in list */
            int max_y_d, max_x_d;
            getmaxyx(stdscr, max_y_d, max_x_d);
            (void)max_x_d;
            int half = (max_y_d - 3) / 2;
            g_tui.selected += half;
            if (g_tui.selected >= (int)count) g_tui.selected = (int)count - 1;
            if (g_tui.selected < 0) g_tui.selected = 0;
            /* Adjust scroll */
            int height_d = max_y_d - 3;
            if (g_tui.selected >= g_tui.offset + height_d)
                g_tui.offset = g_tui.selected - height_d + 1;
            g_tui.top_offset = 0;
            g_tui.preview_match_line = -1;
            break;
        }

        case CTRL('u'): {
            /* Half-page up in list */
            int max_y_u, max_x_u;
            getmaxyx(stdscr, max_y_u, max_x_u);
            (void)max_x_u;
            int half_u = (max_y_u - 3) / 2;
            g_tui.selected -= half_u;
            if (g_tui.selected < 0) g_tui.selected = 0;
            if (g_tui.selected < g_tui.offset)
                g_tui.offset = g_tui.selected;
            g_tui.top_offset = 0;
            g_tui.preview_match_line = -1;
            break;
        }

        case 'j':
        case KEY_DOWN:
            if (g_tui.selected < (int)count - 1) {
                g_tui.selected++;
                /* Scroll list if needed */
                {
                    int max_y, max_x;
                    getmaxyx(stdscr, max_y, max_x);
                    (void)max_x;
                    int height = max_y - 3;
                    if (g_tui.selected >= g_tui.offset + height) g_tui.offset++;
                }
                g_tui.top_offset = 0;
                /* Auto-jump to first match in new entry */
                {
                    KB_Entry *pe = get_preview_entry();
                    if (pe && pe->raw_content && g_tui.search_query && *g_tui.search_query) {
                        if (!match_cache_valid(pe->id, g_tui.search_query))
                            build_match_cache(pe->id, pe->raw_content, g_tui.search_query);
                        int found = find_match_line(pe->raw_content, g_tui.search_query, -1, +1);
                        g_tui.preview_match_line = found;
                        {   int _my2, _mx2;
                            getmaxyx(stdscr, _my2, _mx2);
                            (void)_mx2;
                            int _h = (_my2 - 3) / 2;
                            g_tui.top_offset = found > _h ? found - _h : 0;
                        }
                    } else {
                        g_tui.preview_match_line = -1;
                    }
                }
            }
            break;

        case 'k':
        case KEY_UP:
            if (g_tui.selected > 0) {
                g_tui.selected--;
                if (g_tui.selected < g_tui.offset) g_tui.offset = g_tui.selected;
                g_tui.top_offset = 0;
                /* Auto-jump to first match in new entry */
                {
                    KB_Entry *pe = get_preview_entry();
                    if (pe && pe->raw_content && g_tui.search_query && *g_tui.search_query) {
                        if (!match_cache_valid(pe->id, g_tui.search_query))
                            build_match_cache(pe->id, pe->raw_content, g_tui.search_query);
                        int found = find_match_line(pe->raw_content, g_tui.search_query, -1, +1);
                        g_tui.preview_match_line = found;
                        {   int _my2, _mx2;
                            getmaxyx(stdscr, _my2, _mx2);
                            (void)_mx2;
                            int _h = (_my2 - 3) / 2;
                            g_tui.top_offset = found > _h ? found - _h : 0;
                        }
                    } else {
                        g_tui.preview_match_line = -1;
                    }
                }
            }
            break;

        case 'g':
            /* Go to top */
            g_tui.selected = 0;
            g_tui.offset = 0;
            g_tui.top_offset = 0;
            break;

        case 'G':
            /* Go to bottom */
            g_tui.selected = (int)count - 1;
            g_tui.offset = (int)count - 10;
            if (g_tui.offset < 0) g_tui.offset = 0;
            g_tui.top_offset = 0;
            break;

        case ' ':
        case KEY_NPAGE:
        case CTRL('f'):
            /* Page down in preview */
            g_tui.top_offset += 10;
            break;

        case KEY_PPAGE:
        case CTRL('b'):
            /* Page up in preview */
            g_tui.top_offset -= 10;
            if (g_tui.top_offset < 0) g_tui.top_offset = 0;
            break;

        case '/':
            do_search();
            break;

        case 'n': {
            /* Next match: try current note, then jump to next note */
            if (!g_tui.search_query || !*g_tui.search_query) break;
            KB_Entry *pe = get_preview_entry();
            if (pe && pe->raw_content) {
                if (!match_cache_valid(pe->id, g_tui.search_query))
                    build_match_cache(pe->id, pe->raw_content, g_tui.search_query);
                int from = g_tui.preview_match_line;
                int found = find_match_line(pe->raw_content, g_tui.search_query, from, +1);
                if (found >= 0 && found > from) {
                    /* Found next match in current note */
                    g_tui.preview_match_line = found;
                    {   int _my, _mx;
                            getmaxyx(stdscr, _my, _mx);
                            (void)_mx;
                            int _half = (_my - 3) / 2;
                            g_tui.top_offset = found > _half ? found - _half : 0;
                        }
                    break;
                }
            }
            /* No more forward matches - jump to next note with matches */
            {
                size_t cnt = effective_count();
                for (size_t i = 1; i < cnt; i++) {
                    int idx = (g_tui.selected + (int)i) % (int)cnt;
                    KB_Entry *entry = effective_entry_at(idx);
                    if (entry && entry->raw_content) {
                        build_match_cache(entry->id, entry->raw_content, g_tui.search_query);
                        int found = find_match_line(entry->raw_content, g_tui.search_query, -1, +1);
                        if (found >= 0) {
                            g_tui.selected = idx;
                            /* Adjust left pane scroll */
                            int my, mx;
                            getmaxyx(stdscr, my, mx);
                            (void)mx;
                            int h = my - 3;
                            if (g_tui.selected >= g_tui.offset + h)
                                g_tui.offset = g_tui.selected - h + 1;
                            if (g_tui.selected < g_tui.offset)
                                g_tui.offset = g_tui.selected;
                            g_tui.preview_match_line = found;
                            {   int _my, _mx;
                            getmaxyx(stdscr, _my, _mx);
                            (void)_mx;
                            int _half = (_my - 3) / 2;
                            g_tui.top_offset = found > _half ? found - _half : 0;
                        }
                            break;
                        }
                    }
                }
            }
            break;
        }

        case 'N': {
            /* Previous match: try current note, then jump to previous note */
            if (!g_tui.search_query || !*g_tui.search_query) break;
            KB_Entry *pe = get_preview_entry();
            if (pe && pe->raw_content) {
                if (!match_cache_valid(pe->id, g_tui.search_query))
                    build_match_cache(pe->id, pe->raw_content, g_tui.search_query);
                int from = g_tui.preview_match_line >= 0 ? g_tui.preview_match_line : 0;
                int found = find_match_line(pe->raw_content, g_tui.search_query, from, -1);
                if (found >= 0 && found < from) {
                    /* Found previous match in current note */
                    g_tui.preview_match_line = found;
                    {   int _my, _mx;
                            getmaxyx(stdscr, _my, _mx);
                            (void)_mx;
                            int _half = (_my - 3) / 2;
                            g_tui.top_offset = found > _half ? found - _half : 0;
                        }
                    break;
                }
            }
            /* No more backward matches - jump to previous note with matches */
            {
                size_t cnt = effective_count();
                for (size_t i = 1; i < cnt; i++) {
                    int idx = ((int)g_tui.selected - (int)i + (int)cnt) % (int)cnt;
                    KB_Entry *entry = effective_entry_at(idx);
                    if (entry && entry->raw_content) {
                        /* Find LAST match in this note */
                        build_match_cache(entry->id, entry->raw_content, g_tui.search_query);
                        int found = find_match_line(entry->raw_content, g_tui.search_query, 999999, -1);
                        if (found >= 0) {
                            g_tui.selected = idx;
                            int my, mx;
                            getmaxyx(stdscr, my, mx);
                            (void)mx;
                            int h = my - 3;
                            if (g_tui.selected >= g_tui.offset + h)
                                g_tui.offset = g_tui.selected - h + 1;
                            if (g_tui.selected < g_tui.offset)
                                g_tui.offset = g_tui.selected;
                            g_tui.preview_match_line = found;
                            {   int _my, _mx;
                            getmaxyx(stdscr, _my, _mx);
                            (void)_mx;
                            int _half = (_my - 3) / 2;
                            g_tui.top_offset = found > _half ? found - _half : 0;
                        }
                            break;
                        }
                    }
                }
            }
            break;
        }

        case '\n':
        case '\r':
        case KEY_ENTER:
            /* Open in reader mode */
            if (g_tui.selected >= 0 && (size_t)g_tui.selected < count) {
                KB_Entry *entry = effective_entry_at(g_tui.selected);

                if (entry && entry->raw_content) {
                    g_tui.mode = MODE_READER;
                    reader_init(entry);
                    /* Carry over list-mode search into reader */
                    if (g_tui.search_query && *g_tui.search_query) {
                        ReaderState *r = &g_tui.reader;
                        free(r->search_pattern);
                        r->search_pattern = kb_strdup(g_tui.search_query);
                        if (g_tui.preview_match_line >= 0) {
                            r->scroll_y = g_tui.preview_match_line;
                            r->cursor_y = g_tui.preview_match_line;
                        }
                    }
                    reader_run();
                    reader_cleanup();
                }
            }
            break;

        case '?':
            /* Show help */
            {
                endwin();
                printf("\n");
                printf("╔══════════════════════════════════════════════════════════════╗\n");
                printf("║                    kbfs - 鍵盤快速鍵                         ║\n");
                printf("╠══════════════════════════════════════════════════════════════╣\n");
                printf("║  導航                                                        ║\n");
                printf("║    j, ↓     向下選擇項目                                     ║\n");
                printf("║    k, ↑     向上選擇項目                                     ║\n");
                printf("║    g        跳到第一個項目                                   ║\n");
                printf("║    G        跳到最後一個項目                                 ║\n");
                printf("║                                                              ║\n");
                printf("║  搜尋                                                        ║\n");
                printf("║    /        輸入搜尋關鍵字                                   ║\n");
                printf("║    r, R     清除搜尋結果                                     ║\n");
                printf("║                                                              ║\n");
                printf("║  預覽                                                        ║\n");
                printf("║    Space    向下捲動預覽                                     ║\n");
                printf("║    PgUp     向上捲動預覽                                     ║\n");
                printf("║                                                              ║\n");
                printf("║  篩選                                                        ║\n");
                printf("║    t        篩選 Topic (再按一次清除)                        ║\n");
                printf("║    T        篩選 Type: knowledge/code/workflow              ║\n");
                printf("║    i        Inbox Review (審核 draft 筆記)                   ║\n");
                printf("║                                                              ║\n");
                printf("║  多選                                                        ║\n");
                printf("║    m        勾選/取消勾選項目                                ║\n");
                printf("║    A        勾選所有可見項目（依 topic/search 篩選）         ║\n");
                printf("║             再按一次 A 取消全部勾選                          ║\n");
                printf("║    M        清除所有勾選                                     ║\n");
                printf("║                                                              ║\n");
                printf("║  動作                                                        ║\n");
                printf("║    Enter    進入 Markdown Reader 模式                        ║\n");
                printf("║    L        啟動 Claude Code (多選時載入全部勾選的筆記)      ║\n");
                printf("║    R        恢復暫停中的 Claude Code                         ║\n");
                printf("║    d        刪除選中的筆記                                   ║\n");
                printf("║    q, Q     退出 mdkb                                        ║\n");
                printf("║    ?        顯示此幫助畫面                                   ║\n");
                printf("╚══════════════════════════════════════════════════════════════╝\n");
                printf("\n按 Enter 繼續...");
                getchar();
                refresh_display();
            }
            break;

        case 'm': {
            /* Toggle mark on current entry for multi-select */
            KB_Entry *mentry = NULL;
            if (g_tui.selected >= 0 && (size_t)g_tui.selected < count) {
                mentry = effective_entry_at(g_tui.selected);
            }
            if (mentry && g_tui.marked && mentry->id < g_tui.index->entry_capacity) {
                g_tui.marked[mentry->id] = !g_tui.marked[mentry->id];
                g_tui.mark_count += g_tui.marked[mentry->id] ? 1 : -1;
                /* Move cursor down after marking */
                if (g_tui.selected < (int)count - 1) g_tui.selected++;
            }
            break;
        }

        case 'M':
            /* Clear all marks */
            if (g_tui.marked) {
                memset(g_tui.marked, 0, g_tui.index->entry_capacity * sizeof(bool));
                g_tui.mark_count = 0;
            }
            break;

        case 'A': {
            /* Mark all visible entries (respects active topic/type/search filter).
             * If all visible entries are already marked, unmark them all instead. */
            if (!g_tui.marked) break;
            size_t visible = effective_count();
            int all_marked = 1;
            for (size_t vi = 0; vi < visible; vi++) {
                KB_Entry *ve = effective_entry_at((int)vi);
                if (ve && ve->id < g_tui.index->entry_capacity && !g_tui.marked[ve->id]) {
                    all_marked = 0;
                    break;
                }
            }
            for (size_t vi = 0; vi < visible; vi++) {
                KB_Entry *ve = effective_entry_at((int)vi);
                if (!ve || ve->id >= g_tui.index->entry_capacity) continue;
                bool was = g_tui.marked[ve->id];
                bool want = !all_marked;
                if (was != want) {
                    g_tui.marked[ve->id] = want;
                    g_tui.mark_count += want ? 1 : -1;
                }
            }
            break;
        }

        case 'L': {
            /* Suspended session with loaded marks — smart dispatch */
            if (g_claude_pid > 0 && g_tui.loaded_mark_count > 0) {
                if (marks_unchanged()) {
                    /* No change — just resume */
                    resume_claude();
                } else if (marks_have_removals()) {
                    /* Removed loaded files — warn: will start new session */
                    int max_y, max_x;
                    getmaxyx(stdscr, max_y, max_x);
                    (void)max_x;
                    attron(A_BOLD | COLOR_PAIR(CP_TAG));
                    mvprintw(max_y - 1, 1,
                        "Loaded files removed. L will start a new session. Press y to confirm, any other key to cancel.");
                    attroff(A_BOLD | COLOR_PAIR(CP_TAG));
                    refresh();
                    int confirm = getch();
                    if (confirm == 'y' || confirm == 'Y') {
                        kill_stopped_claude();
                        if (g_tui.mark_count > 0)
                            launch_claude_multi();
                        else
                            clear_loaded_marks();
                    }
                } else {
                    /* Only additions — continue session with new files */
                    launch_claude_continue();
                }
                refresh_display();
                break;
            }

            /* No suspended session — fresh multi-select launch */
            if (g_tui.mark_count > 0) {
                launch_claude_multi();
                refresh_display();
                break;
            }

            /* Single-select: resume session or inject single note */
            KB_Entry *lentry = NULL;
            if (g_tui.selected >= 0 && (size_t)g_tui.selected < count) {
                lentry = effective_entry_at(g_tui.selected);
            }
            if (lentry) {
                if (lentry->session_id && lentry->session_id[0]) {
                    char *sid_cwd = find_session_cwd(lentry->session_id);
                    launch_claude(lentry->session_id, sid_cwd);
                    free(sid_cwd);
                    refresh_display();
                } else {
                    launch_claude(NULL, NULL);
                    refresh_display();
                }
            }
            break;
        }


        case 'd': {
            /* Delete selected note */
            KB_Entry *dentry = NULL;
            if (g_tui.selected >= 0 && (size_t)g_tui.selected < count) {
                dentry = effective_entry_at(g_tui.selected);
            }
            if (dentry && dentry->path && g_tui.mdkb_root) {
                /* Confirm deletion */
                int max_y_d, max_x_d;
                getmaxyx(stdscr, max_y_d, max_x_d);
                (void)max_x_d;
                attron(A_BOLD | COLOR_PAIR(CP_TAG));
                mvprintw(max_y_d - 1, 1, "Delete this note? (y/N) ");
                attroff(A_BOLD | COLOR_PAIR(CP_TAG));
                int confirm = getch();
                if (confirm == 'y' || confirm == 'Y') {
                    /* entry->path is relative to mdkb_root (e.g. "notes/claude/...") */
                    char *full = kb_path_join(g_tui.mdkb_root, dentry->path);
                    if (unlink(full) == 0) {
                        /* Clear search results (stale after index change) */
                        if (g_tui.results) {
                            mdmdkb_search_free(g_tui.results);
                            g_tui.results = NULL;
                            free(g_tui.search_query);
                            g_tui.search_query = NULL;
                        }
                        invalidate_filter_cache();
                        /* Remove from index */
                        for (size_t di = 0; di < g_tui.index->entry_count; di++) {
                            if (g_tui.index->entries[di].id == dentry->id) {
                                mdkb_entry_free(&g_tui.index->entries[di]);
                                memmove(&g_tui.index->entries[di],
                                        &g_tui.index->entries[di + 1],
                                        (g_tui.index->entry_count - di - 1) * sizeof(KB_Entry));
                                g_tui.index->entry_count--;
                                break;
                            }
                        }
                        /* Adjust selection */
                        if (g_tui.selected >= (int)g_tui.index->entry_count && g_tui.selected > 0)
                            g_tui.selected--;
                    }
                    free(full);
                }
            }
            break;
        }

        case 'Y': {
            /* Yank entire current entry content to clipboard */
            KB_Entry *yentry = NULL;
            if (g_tui.selected >= 0 && (size_t)g_tui.selected < count) {
                yentry = effective_entry_at(g_tui.selected);
            }
            if (yentry && yentry->raw_content) {
                size_t ylen = strlen(yentry->raw_content);
                char tmp_path[] = "/tmp/mdkb_yank_XXXXXX";
                int tmp_fd = mkstemp(tmp_path);
                if (tmp_fd >= 0) {
                    (void)write(tmp_fd, yentry->raw_content, ylen);
                    close(tmp_fd);
                    yank_to_clipboard(tmp_path);
                    unlink(tmp_path);
                }
            }
            break;
        }

        case 'E': {
            /* Meta-edit: title / topic / tags for selected note (knowledge mode only) */
            if (g_tui.archive_mode) break;
            KB_Entry *eentry = NULL;
            if (g_tui.selected >= 0 && (size_t)g_tui.selected < count)
                eentry = effective_entry_at(g_tui.selected);
            if (!eentry || !eentry->path || !g_tui.mdkb_root) break;

            char *efull = kb_path_join(g_tui.mdkb_root, eentry->path);

            /* Step 1: edit title */
            char *new_title = do_title_edit_popup(eentry->title);
            if (new_title) {
                if (update_fm_field_on_disk(efull, "title", new_title) == 0) {
                    free(eentry->title);
                    eentry->title = new_title;
                } else {
                    free(new_title);
                }
            }

            /* Step 2: edit topic (may move file) */
            char *new_topic = do_topic_filter_popup();
            if (new_topic) {
                /* Extract repo from current path: repo/topic/file.md */
                char *cur_path = kb_strdup(eentry->path);
                char *slash1 = strchr(cur_path, '/');
                char *cur_repo = NULL;
                const char *cur_basename = NULL;
                if (slash1) {
                    *slash1 = '\0';
                    cur_repo = kb_strdup(cur_path);
                    char *slash2 = strrchr(slash1 + 1, '/');
                    cur_basename = slash2 ? slash2 + 1 : slash1 + 1;
                } else {
                    cur_repo = kb_strdup("knowledge");
                    cur_basename = cur_path;
                }
                free(cur_path);

                /* Build destination path */
                char *dest_dir  = kb_path_join(g_tui.mdkb_root, cur_repo);
                char *dest_dir2 = kb_path_join(dest_dir, new_topic);
                mkdir(dest_dir, 0755);
                mkdir(dest_dir2, 0755);
                char *dest_full = kb_path_join(dest_dir2, cur_basename);

                update_fm_field_on_disk(efull, "topic", new_topic);
                if (rename(efull, dest_full) == 0) {
                    free(efull);
                    efull = dest_full;
                    /* Update in-memory path */
                    char new_rel[4096];
                    snprintf(new_rel, sizeof(new_rel), "%s/%s/%s",
                             cur_repo, new_topic, cur_basename);
                    free(eentry->path);
                    eentry->path = kb_strdup(new_rel);
                    invalidate_filter_cache();
                } else {
                    free(dest_full);
                }
                free(dest_dir);
                free(dest_dir2);
                free(cur_repo);
                free(new_topic);
            }

            /* Step 3: edit tags */
            do_tag_edit_popup(eentry, efull);

            free(efull);
            break;
        }

        case 'R':
            /* Resume paused Claude Code */
            if (g_claude_pid > 0) {
                resume_claude();
                refresh_display();
                break;
            }
            /* fall through to reset search if no Claude paused */
            __attribute__((fallthrough));
        case 't': {
            /* Topic filter (knowledge mode only) */
            if (g_tui.archive_mode) break;
            if (g_tui.topic_filter) {
                set_topic_filter(NULL);
            } else {
                char *sel = do_topic_filter_popup();
                if (sel) {
                    set_topic_filter(sel);
                    free(sel);
                }
            }
            break;
        }

        case 'T': {
            /* Type filter (knowledge mode only) */
            if (g_tui.archive_mode) break;
            if (g_tui.type_filter) {
                set_type_filter(NULL);
            } else {
                char *sel = do_type_filter_popup();
                if (sel) {
                    set_type_filter(sel);
                    free(sel);
                }
            }
            break;
        }

        case '#': {
            /* Tag filter — multi-select popup (knowledge mode only) */
            if (g_tui.archive_mode) break;
            do_tag_filter_popup();
            break;
        }

        case 'i': {
            /* Inbox review (knowledge mode only) */
            if (g_tui.archive_mode) break;
            do_inbox_review();
            refresh_display();
            break;
        }

        case 'r':
            /* Reset search, topic filter, and type filter */
            if (g_tui.search_query) {
                free(g_tui.search_query);
                g_tui.search_query = NULL;
            }
            if (g_tui.results) {
                mdmdkb_search_free(g_tui.results);
                g_tui.results = NULL;
                invalidate_filter_cache();
            }
            set_topic_filter(NULL);
            set_type_filter(NULL);
            clear_tag_filters();
            break;
    }
}

/* ============================================================================
 * READER MODE - Vim-like Markdown Reader
 * ============================================================================ */

/* Split content into lines, optionally filtering out Human sections */
static void reader_split_lines(ReaderState *reader, const char *content, int ai_only) {
    reader->line_count = 0;
    reader->in_code_block = 0;

    if (!content) return;

    const char *p = content;
    int in_human = 0;  /* Track whether we're inside a ## Human section */

    while (*p) {
        const char *line_end = strchr(p, '\n');
        size_t len = line_end ? (size_t)(line_end - p) : strlen(p);

        /* Detect section boundaries for AI-only filtering */
        if (ai_only) {
            if (len >= 8 && strncmp(p, "## Human", 8) == 0 &&
                (len == 8 || p[8] == '\r' || p[8] == '\n' || p[8] == ' ')) {
                in_human = 1;
                /* Skip this line (the ## Human header itself) */
                if (line_end) p = line_end + 1;
                else break;
                continue;
            } else if (len >= 2 && strncmp(p, "##", 2) == 0 && in_human) {
                /* Any new ## header ends the Human section */
                in_human = 0;
            }

            if (in_human) {
                if (line_end) p = line_end + 1;
                else break;
                continue;
            }
        }

        /* Expand capacity if needed */
        if (reader->line_count >= reader->line_capacity) {
            reader->line_capacity = reader->line_capacity ? reader->line_capacity * 2 : 256;
            reader->lines = realloc(reader->lines, reader->line_capacity * sizeof(char *));
        }

        reader->lines[reader->line_count] = kb_strndup(p, len);
        reader->line_count++;

        if (line_end) p = line_end + 1;
        else break;
    }
}

/* Initialize reader with entry */
static void reader_init(KB_Entry *entry) {
    ReaderState *r = &g_tui.reader;

    /* Free old lines */
    if (r->lines) {
        for (size_t i = 0; i < r->line_count; i++) {
            free(r->lines[i]);
        }
        free(r->lines);
    }
    free(r->search_pattern);

    memset(r, 0, sizeof(ReaderState));
    r->entry = entry;
    r->search_direction = 1;
    r->preview_mode = 1;  /* Default to preview mode */

    if (entry && entry->raw_content) {
        reader_split_lines(r, entry->raw_content, r->ai_only_mode);
    }
}

/* Cleanup reader */
static void reader_cleanup(void) {
    ReaderState *r = &g_tui.reader;
    if (r->lines) {
        for (size_t i = 0; i < r->line_count; i++) {
            free(r->lines[i]);
        }
        free(r->lines);
        r->lines = NULL;
    }
    free(r->search_pattern);
    r->search_pattern = NULL;
    r->line_count = 0;
    r->entry = NULL;
}

/* Check if position is in selection */
static int reader_is_selected(int line_y, int line_x) {
    ReaderState *r = &g_tui.reader;
    if (!r->visual_mode) return 0;

    int start_y = r->visual_start_y;
    int start_x = r->visual_start_x;
    int end_y = r->cursor_y;
    int end_x = r->cursor_x;

    /* Normalize selection - ensure start is before end */
    if (start_y > end_y || (start_y == end_y && start_x > end_x)) {
        int tmp = start_y; start_y = end_y; end_y = tmp;
        tmp = start_x; start_x = end_x; end_x = tmp;
    }

    /* Check if position is within selection bounds */
    if (line_y < start_y || line_y > end_y) return 0;
    if (line_y == start_y && line_x < start_x) return 0;
    if (line_y == end_y && line_x > end_x) return 0;
    return 1;
}

/* Draw reader screen */
static void reader_draw(void) {
    ReaderState *r = &g_tui.reader;
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);

    clear();

    /* Header */
    attron(A_BOLD | COLOR_PAIR(CP_HEADER));
    mvprintw(0, 0, " %s", r->entry && r->entry->title ? r->entry->title : "Reader");
    if (r->preview_mode) {
        printw(" [PREVIEW]");
    }
    if (r->visual_mode) {
        printw(" [VISUAL]");
    } else if (r->search_pattern) {
        printw(" [/%s]", r->search_pattern);
    }
    if (r->raw_mode) {
        printw(" [RAW]");
    }
    if (r->ai_only_mode) {
        printw(" [AI-ONLY]");
    }
    attroff(A_BOLD | COLOR_PAIR(CP_HEADER));
    ui_hline(1, 0, max_x);

    /* Content area */
    int content_y = 2;
    int content_height = max_y - content_y - 1;

    /* Preview mode: render like list view right pane (whole line rendering) */
    int margin = 1;  /* left margin for readability */
    if (r->preview_mode) {
        r->in_code_block = 0;
        for (int row = 0; row < content_height; row++) {
            int line_idx = r->scroll_y + row;
            if ((size_t)line_idx >= r->line_count) break;

            char *line = r->lines[line_idx];
            if (!line) continue;

            int screen_y = content_y + row;
            move(screen_y, margin);
            r->in_code_block = render_markdown_line(line, screen_y, margin, max_x - margin - 1, r->in_code_block);

            /* Overlay search match highlight with gutter indicators */
            if (r->search_pattern && *r->search_pattern) {
                size_t ll = strlen(line);
                int is_match = (line_match_offset(line, ll, r->search_pattern) >= 0);
                int is_current = (line_idx == r->cursor_y && is_match);

                if (is_current) {
                    /* Current match: > gutter + highlight */
                    attron(COLOR_PAIR(CP_TAG) | A_BOLD);
                    mvaddch(screen_y, 0, '>');
                    attroff(COLOR_PAIR(CP_TAG) | A_BOLD);
                    mvchgat(screen_y, margin, max_x - margin, A_BOLD, CP_TAG, NULL);
                } else if (is_match) {
                    /* Other match: - gutter */
                    attron(COLOR_PAIR(CP_TAG));
                    mvaddch(screen_y, 0, '-');
                    attroff(COLOR_PAIR(CP_TAG));
                }
            }

            /* Overlay visual selection highlight */
            if (r->visual_mode) {
                int sel_start_y = r->visual_start_y, sel_start_x = r->visual_start_x;
                int sel_end_y   = r->cursor_y,       sel_end_x   = r->cursor_x;
                /* Normalize so start <= end */
                if (sel_start_y > sel_end_y ||
                    (sel_start_y == sel_end_y && sel_start_x > sel_end_x)) {
                    int tmp = sel_start_y; sel_start_y = sel_end_y; sel_end_y = tmp;
                    tmp = sel_start_x; sel_start_x = sel_end_x; sel_end_x = tmp;
                }
                if (line_idx >= sel_start_y && line_idx <= sel_end_y) {
                    int col_start = (line_idx == sel_start_y) ? sel_start_x : 0;
                    int col_end   = (line_idx == sel_end_y)   ? sel_end_x   : max_x - 1;
                    int col_len   = (col_end > col_start) ? col_end - col_start : -1;
                    if (sel_start_y == sel_end_y) {
                        /* Single line: highlight between start_x and end_x */
                        mvchgat(screen_y, col_start, col_len, A_REVERSE, CP_SELECTION, NULL);
                    } else if (line_idx == sel_start_y) {
                        /* First line: highlight from start_x to end of line */
                        mvchgat(screen_y, col_start, -1, A_REVERSE, CP_SELECTION, NULL);
                    } else if (line_idx == sel_end_y) {
                        /* Last line: highlight from 0 to end_x */
                        mvchgat(screen_y, 0, col_end, A_REVERSE, CP_SELECTION, NULL);
                    } else {
                        /* Middle lines: highlight entire line */
                        mvchgat(screen_y, 0, -1, A_REVERSE, CP_SELECTION, NULL);
                    }
                }
            }
        }
        /* Position cursor in preview mode - draw highlighted char */
        int cursor_screen_y = r->cursor_y - r->scroll_y + content_y;
        int cursor_screen_x = r->cursor_x - r->scroll_x;
        if (cursor_screen_y >= content_y && cursor_screen_y < content_y + content_height &&
            cursor_screen_x >= 0 && cursor_screen_x < max_x - 1) {
            attron(COLOR_PAIR(CP_CURSOR));
            mvaddch(cursor_screen_y, cursor_screen_x, ' ');
            attroff(COLOR_PAIR(CP_CURSOR));
            move(cursor_screen_y, cursor_screen_x);
        }
        curs_set(1);
    } else {
        /* Edit mode: render with cursor, selection, etc. */
        r->in_code_block = 0;

    for (int row = 0; row < content_height; row++) {
        int line_idx = r->scroll_y + row;
        if ((size_t)line_idx >= r->line_count) break;

        char *line = r->lines[line_idx];
        if (!line) continue;

        int screen_y = content_y + row;
        int col = 0;

        /* Check for code fence */
        if (strncmp(line, "```", 3) == 0) {
            r->in_code_block = !r->in_code_block;
        }

        /* Raw mode: just display plain text, no rendering at all */
        if (r->raw_mode) {
            const char *p = line + r->scroll_x;
            int x = 0;
            while (*p && x < max_x - 1) {
                int abs_x = r->scroll_x + x;
                int is_cursor = (line_idx == r->cursor_y && abs_x == r->cursor_x);
                int is_selected = reader_is_selected(line_idx, abs_x);

                if (is_cursor) {
                    attron(COLOR_PAIR(CP_CURSOR));
                } else if (is_selected) {
                    attron(COLOR_PAIR(CP_SELECTION));
                }

                mvaddch(screen_y, col, *p);
                attrset(A_NORMAL);

                p++;
                col++;
                x++;
            }

            /* Draw cursor in empty area */
            if (line_idx == r->cursor_y) {
                int cursor_screen_x = r->cursor_x - r->scroll_x;
                if (cursor_screen_x >= 0 && cursor_screen_x < max_x - 1 && cursor_screen_x >= col) {
                    attron(COLOR_PAIR(CP_CURSOR));
                    mvaddch(screen_y, cursor_screen_x, ' ');
                    attroff(COLOR_PAIR(CP_CURSOR));
                }
            }
            continue;
        }

        /* Render line with markdown and selection */
        const char *p = line + r->scroll_x;
        int x = 0;

        while (*p && x < max_x - 1) {
            int abs_x = r->scroll_x + x;
            int is_cursor = (line_idx == r->cursor_y && abs_x == r->cursor_x);
            int is_selected = reader_is_selected(line_idx, abs_x);

            /* Handle attributes */
            if (is_cursor) {
                attron(COLOR_PAIR(CP_CURSOR));
            } else if (is_selected) {
                attron(COLOR_PAIR(CP_SELECTION));
            } else if (r->in_code_block || strncmp(p, "```", 3) == 0) {
                attron(COLOR_PAIR(CP_CODE));
            } else if (*p == '#') {
                int level = 0;
                while (p[level] == '#' && level < 6) level++;
                if (col == 0 && level > 0) {
                    attron(A_BOLD | COLOR_PAIR(CP_HEADER));
                }
            } else if (*p == '*' && p[1] == '*') {
                attron(A_BOLD);
            } else if ((*p == '*' || *p == '_') && col > 0 && *(p-1) != *p) {
                attron(A_UNDERLINE | COLOR_PAIR(CP_EMPHASIS));
            } else if (*p == '`') {
                attron(COLOR_PAIR(CP_CODE));
            } else if (*p == '>') {
                attron(COLOR_PAIR(CP_QUOTE));
            }

            mvaddch(screen_y, col, *p);

            /* Reset attributes */
            attrset(A_NORMAL);

            p++;
            col++;
            x++;
        }

        /* Draw cursor if it's on this line but past the text (empty area) */
        if (line_idx == r->cursor_y) {
            int cursor_screen_x = r->cursor_x - r->scroll_x;
            if (cursor_screen_x >= 0 && cursor_screen_x < max_x - 1 && cursor_screen_x >= col) {
                attron(COLOR_PAIR(CP_CURSOR));
                mvaddch(screen_y, cursor_screen_x, ' ');
                attroff(COLOR_PAIR(CP_CURSOR));
            }
        }
        }
    }

    /* Status bar */
    attron(A_REVERSE);
    mvwhline(stdscr, max_y - 1, 0, ' ', max_x);
    if (r->preview_mode) {
        mvprintw(max_y - 1, 1, "%zu lines | Y:copy-all C:ai-only i:edit q:quit /:search",
                 r->line_count);
    } else {
        mvprintw(max_y - 1, 1, "%d/%zu | %d,%d | h/j/k/l:move v:visual y:yank Y:copy-all C:ai-only i:preview /:search q:quit",
                 r->cursor_y + 1, r->line_count, r->cursor_y + 1, r->cursor_x + 1);
    }
    attroff(A_REVERSE);

    /* Always show cursor before refresh */
    curs_set(1);
    refresh();
}

/* Ensure cursor is visible */
static void reader_ensure_cursor_visible(int max_y, int max_x) {
    ReaderState *r = &g_tui.reader;
    int content_height = max_y - 3;

    /* Vertical scrolling */
    if (r->cursor_y < r->scroll_y) {
        r->scroll_y = r->cursor_y;
    } else if (r->cursor_y >= r->scroll_y + content_height) {
        r->scroll_y = r->cursor_y - content_height + 1;
    }

    /* Horizontal scrolling */
    if (r->cursor_x < r->scroll_x) {
        r->scroll_x = r->cursor_x;
    } else if (r->cursor_x >= r->scroll_x + max_x - 1) {
        r->scroll_x = r->cursor_x - max_x + 2;
    }
}

/* Get line length */
static int reader_line_length(int y) {
    ReaderState *r = &g_tui.reader;
    if (y < 0 || (size_t)y >= r->line_count) return 0;
    return r->lines[y] ? strlen(r->lines[y]) : 0;
}

/* Move cursor */
static void reader_move_cursor(int dy, int dx) {
    ReaderState *r = &g_tui.reader;
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);

    int new_y = r->cursor_y + dy;
    int new_x = r->cursor_x + dx;

    /* Clamp Y */
    if (new_y < 0) new_y = 0;
    if ((size_t)new_y >= r->line_count) new_y = r->line_count - 1;

    /* Clamp X to line length */
    int line_len = reader_line_length(new_y);
    if (new_x < 0) new_x = 0;
    if (new_x > line_len) new_x = line_len;

    r->cursor_y = new_y;
    r->cursor_x = new_x;

    reader_ensure_cursor_visible(max_y, max_x);
}

/* Search in reader */
static void reader_search(void) {
    ReaderState *r = &g_tui.reader;

    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);

    /* Get search pattern */
    echo();
    curs_set(1);

    char pattern[256] = {0};
    mvprintw(max_y - 1, 1, "/");
    getnstr(pattern, sizeof(pattern) - 1);

    noecho();
    curs_set(0);

    if (strlen(pattern) > 0) {
        free(r->search_pattern);
        r->search_pattern = kb_strdup(pattern);

        /* Find first match (case-insensitive) */
        for (size_t y = 0; y < r->line_count; y++) {
            char *line = r->lines[y];
            if (!line) continue;

            int off = line_match_offset(line, strlen(line), pattern);
            if (off >= 0) {
                if (r->preview_mode) {
                    int ch = max_y - 3;
                    r->scroll_y = (int)y > ch / 2 ? (int)y - ch / 2 : 0;
                    r->cursor_y = y;
                } else {
                    r->cursor_y = y;
                    r->cursor_x = off;
                    reader_ensure_cursor_visible(max_y, max_x);
                }
                break;
            }
        }
    }
}

/* Find next search result */
static void reader_search_next(void) {
    ReaderState *r = &g_tui.reader;
    if (!r->search_pattern) return;

    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);

    int start_y = r->cursor_y;
    if (r->search_direction > 0) start_y++;
    else start_y--;

    if (r->search_direction > 0) {
        /* Search forward */
        for (size_t y = start_y; y < r->line_count; y++) {
            char *line = r->lines[y];
            if (!line) continue;

            int off = line_match_offset(line, strlen(line), r->search_pattern);
            if (off >= 0) {
                if (r->preview_mode) {
                    int ch = max_y - 3;
                    r->scroll_y = (int)y > ch / 2 ? (int)y - ch / 2 : 0;
                    r->cursor_y = y;
                } else {
                    r->cursor_y = y;
                    r->cursor_x = off;
                    reader_ensure_cursor_visible(max_y, max_x);
                }
                return;
            }
        }
    } else {
        /* Search backward */
        for (int y = start_y; y >= 0; y--) {
            char *line = r->lines[y];
            if (!line) continue;

            /* Find last match on this line (case-insensitive) */
            size_t ll = strlen(line);
            size_t qlen = strlen(r->search_pattern);
            int last_off = -1;
            for (size_t si = 0; si + qlen <= ll; si++) {
                int off = line_match_offset(line + si, ll - si, r->search_pattern);
                if (off < 0) break;
                last_off = (int)(si + (size_t)off);
                si += (size_t)off;
            }
            if (last_off >= 0) {
                if (r->preview_mode) {
                    int ch = max_y - 3;
                    r->scroll_y = y > ch / 2 ? y - ch / 2 : 0;
                    r->cursor_y = y;
                } else {
                    r->cursor_y = y;
                    r->cursor_x = last_off;
                    reader_ensure_cursor_visible(max_y, max_x);
                }
                return;
            }
        }
    }
}

/* Copy the content of tmp_path to clipboard using best available method:
 *  1. xclip/xsel  (X11 - DISPLAY is set)
 *  2. wl-copy     (Wayland - WAYLAND_DISPLAY is set)
 *  3. OSC 52      (terminal escape sequence - works over SSH/headless) */
static void yank_to_clipboard(const char *tmp_path) {
    char cmd[1024];
    const char *display = getenv("DISPLAY");
    const char *wayland = getenv("WAYLAND_DISPLAY");

    if (display && *display) {
        snprintf(cmd, sizeof(cmd),
                 "xclip -selection clipboard < %s 2>/dev/null"
                 " || xsel --clipboard --input < %s 2>/dev/null",
                 tmp_path, tmp_path);
        (void)system(cmd);
    } else if (wayland && *wayland) {
        snprintf(cmd, sizeof(cmd), "wl-copy < %s 2>/dev/null", tmp_path);
        (void)system(cmd);
    } else {
        /* OSC 52: instruct the terminal emulator to set its clipboard.
         * Supported by iTerm2, Kitty, WezTerm, xterm (allowWindowOps=true),
         * and most modern terminals even over SSH. */
        snprintf(cmd, sizeof(cmd),
                 "printf '\\033]52;c;%%s\\007'"
                 " \"$(base64 -w 0 < %s 2>/dev/null || base64 < %s)\""
                 " > /dev/tty 2>/dev/null",
                 tmp_path, tmp_path);
        (void)system(cmd);
    }
}

/* Yank (copy) selection to clipboard */
static void reader_yank(void) {
    ReaderState *r = &g_tui.reader;
    if (!r->visual_mode) return;

    int start_y = r->visual_start_y;
    int start_x = r->visual_start_x;
    int end_y = r->cursor_y;
    int end_x = r->cursor_x;

    /* Normalize */
    if (start_y > end_y || (start_y == end_y && start_x > end_x)) {
        int tmp = start_y; start_y = end_y; end_y = tmp;
        tmp = start_x; start_x = end_x; end_x = tmp;
    }

    /* Build selection string */
    size_t bufsize = 4096;
    char *buf = malloc(bufsize);
    size_t len = 0;
    buf[0] = '\0';

    for (int y = start_y; y <= end_y && (size_t)y < r->line_count; y++) {
        char *line = r->lines[y];
        if (!line) continue;

        int line_len = strlen(line);
        int sel_start = (y == start_y) ? start_x : 0;
        int sel_end = (y == end_y) ? end_x : line_len;

        if (sel_end > line_len) sel_end = line_len;
        if (sel_start < 0) sel_start = 0;

        int sel_len = sel_end - sel_start;
        if (sel_len > 0) {
            /* Ensure buffer has space */
            if (len + sel_len + 2 > bufsize) {
                bufsize = len + sel_len + 4096;
                buf = realloc(buf, bufsize);
            }
            memcpy(buf + len, line + sel_start, sel_len);
            len += sel_len;
        }

        if (y < end_y) {
            buf[len++] = '\n';
        }
    }
    buf[len] = '\0';

    /* Copy to clipboard */
    char tmp_path[] = "/tmp/mdkb_yank_XXXXXX";
    int tmp_fd = mkstemp(tmp_path);
    if (tmp_fd >= 0) {
        (void)write(tmp_fd, buf, len);
        close(tmp_fd);
        yank_to_clipboard(tmp_path);
        unlink(tmp_path);
    }

    free(buf);

    /* Exit visual mode */
    r->visual_mode = 0;
}

/* Yank all visible lines to clipboard (respects ai_only_mode filtering) */
static void reader_yank_all(void) {
    ReaderState *r = &g_tui.reader;
    if (r->line_count == 0) return;

    size_t total = 0;
    for (size_t i = 0; i < r->line_count; i++) {
        if (r->lines[i]) total += strlen(r->lines[i]) + 1;
    }

    char *buf = malloc(total + 1);
    if (!buf) return;
    size_t len = 0;

    for (size_t i = 0; i < r->line_count; i++) {
        if (!r->lines[i]) continue;
        size_t ll = strlen(r->lines[i]);
        memcpy(buf + len, r->lines[i], ll);
        len += ll;
        buf[len++] = '\n';
    }
    buf[len] = '\0';

    char tmp_path[] = "/tmp/mdkb_yank_XXXXXX";
    int tmp_fd = mkstemp(tmp_path);
    if (tmp_fd >= 0) {
        (void)write(tmp_fd, buf, len);
        close(tmp_fd);
        yank_to_clipboard(tmp_path);
        unlink(tmp_path);
    }
    free(buf);
}

/* Run reader main loop */
static void reader_run(void) {
    ReaderState *r = &g_tui.reader;
    int max_y, max_x;

    curs_set(1);

    while (g_tui.mode == MODE_READER && g_tui.running) {
        getmaxyx(stdscr, max_y, max_x);
        reader_draw();

        int key = getch();

        switch (key) {
            case 'i':
                /* Toggle preview/edit mode */
                r->preview_mode = !r->preview_mode;
                break;

            case 'L':
                /* Launch Claude Code from reader mode */
                if (r->entry) {
                    /* Save before reader_cleanup() which NULLs r->entry */
                    char *saved_sid = r->entry->session_id ? kb_strdup(r->entry->session_id) : NULL;
                    reader_cleanup();
                    g_tui.mode = MODE_LIST;
                    if (saved_sid && saved_sid[0]) {
                        launch_claude(saved_sid, NULL);
                    } else {
                        /* No session_id (knowledge note) — launch Claude with content as context */
                        launch_claude(NULL, NULL);
                    }
                    refresh_display();
                    free(saved_sid);
                }
                break;

            case 'C':
                /* Toggle AI-only mode: filter out Human sections */
                r->ai_only_mode = !r->ai_only_mode;
                /* Re-split lines with new filter setting */
                if (r->lines) {
                    for (size_t ci = 0; ci < r->line_count; ci++) free(r->lines[ci]);
                    free(r->lines);
                    r->lines = NULL;
                }
                r->line_count = 0;
                r->line_capacity = 0;
                r->cursor_y = 0;
                r->cursor_x = 0;
                r->scroll_y = 0;
                r->scroll_x = 0;
                if (r->entry && r->entry->raw_content)
                    reader_split_lines(r, r->entry->raw_content, r->ai_only_mode);
                break;

            case 'q':
            case 'Q':
                g_tui.mode = MODE_LIST;
                break;

            case 'h':
            case KEY_LEFT:
                reader_move_cursor(0, -1);
                break;

            case 'j':
            case KEY_DOWN:
                if (r->preview_mode) {
                    /* In preview mode: move cursor and scroll together */
                    if (r->cursor_y + 1 < (int)r->line_count) {
                        r->cursor_y++;
                        reader_ensure_cursor_visible(max_y, max_x);
                    }
                } else {
                    reader_move_cursor(1, 0);
                }
                break;

            case 'k':
            case KEY_UP:
                if (r->preview_mode) {
                    /* In preview mode: move cursor and scroll together */
                    if (r->cursor_y > 0) {
                        r->cursor_y--;
                        reader_ensure_cursor_visible(max_y, max_x);
                    }
                } else {
                    reader_move_cursor(-1, 0);
                }
                break;

            case 'l':
            case KEY_RIGHT:
                reader_move_cursor(0, 1);
                break;

            case '0':
                r->cursor_x = 0;
                reader_ensure_cursor_visible(max_y, max_x);
                break;

            case '$':
                r->cursor_x = reader_line_length(r->cursor_y);
                reader_ensure_cursor_visible(max_y, max_x);
                break;

            case 'g':
                /* Wait for next key - 'g' goes to top */
                {
                    int next = getch();
                    if (next == 'g') {
                        if (r->preview_mode) {
                            r->scroll_y = 0;
                            r->cursor_y = 0;
                        } else {
                            r->cursor_y = 0;
                            r->cursor_x = 0;
                            reader_ensure_cursor_visible(max_y, max_x);
                        }
                    }
                }
                break;

            case 'G':
                if (r->preview_mode) {
                    int content_height = max_y - 3;
                    r->scroll_y = r->line_count - content_height;
                    if (r->scroll_y < 0) r->scroll_y = 0;
                    r->cursor_y = r->scroll_y;
                } else {
                    r->cursor_y = r->line_count - 1;
                    r->cursor_x = 0;
                    reader_ensure_cursor_visible(max_y, max_x);
                }
                break;

            case 'v':
                /* Enter visual mode */
                if (!r->visual_mode) {
                    r->visual_mode = 1;
                    r->visual_start_y = r->cursor_y;
                    r->visual_start_x = r->cursor_x;
                } else {
                    r->visual_mode = 0;
                }
                break;

            case 'y':
                /* Yank selection */
                if (r->visual_mode) {
                    reader_yank();
                }
                break;

            case 'Y':
                /* Yank all visible content to clipboard */
                reader_yank_all();
                break;

            case '/':
                reader_search();
                break;

            case 'n':
                r->search_direction = 1;
                reader_search_next();
                break;

            case 'N':
                r->search_direction = -1;
                reader_search_next();
                break;

            case ']':
                /* Jump to next ## heading */
                {
                    int found = 0;
                    for (int li = r->cursor_y + 1; li < (int)r->line_count; li++) {
                        if (r->lines[li] && strncmp(r->lines[li], "## ", 3) == 0) {
                            r->cursor_y = li;
                            r->cursor_x = 0;
                            reader_ensure_cursor_visible(max_y, max_x);
                            found = 1;
                            break;
                        }
                    }
                    if (!found) {
                        /* Wrap or stay at end */
                        r->cursor_y = (int)r->line_count - 1;
                        reader_ensure_cursor_visible(max_y, max_x);
                    }
                }
                break;

            case '[':
                /* Jump to previous ## heading */
                {
                    for (int li = r->cursor_y - 1; li >= 0; li--) {
                        if (r->lines[li] && strncmp(r->lines[li], "## ", 3) == 0) {
                            r->cursor_y = li;
                            r->cursor_x = 0;
                            reader_ensure_cursor_visible(max_y, max_x);
                            break;
                        }
                    }
                }
                break;

            case CTRL('f'):
            case ' ':
                if (r->preview_mode) {
                    int content_height = max_y - 3;
                    r->scroll_y += content_height;
                    if (r->scroll_y >= (int)r->line_count) r->scroll_y = r->line_count - 1;
                    if (r->scroll_y < 0) r->scroll_y = 0;
                    r->cursor_y = r->scroll_y;  /* keep cursor in sync */
                } else {
                    reader_move_cursor(max_y - 3, 0);
                }
                break;

            case CTRL('b'):
            case KEY_PPAGE:
                if (r->preview_mode) {
                    int content_height = max_y - 3;
                    r->scroll_y -= content_height;
                    if (r->scroll_y < 0) r->scroll_y = 0;
                    r->cursor_y = r->scroll_y;  /* keep cursor in sync */
                } else {
                    reader_move_cursor(-(max_y - 3), 0);
                }
                break;

            case CTRL('d'):
                /* Half page down */
                if (r->preview_mode) {
                    int content_height = max_y - 3;
                    r->scroll_y += content_height / 2;
                    if (r->scroll_y >= (int)r->line_count) r->scroll_y = r->line_count - 1;
                    if (r->scroll_y < 0) r->scroll_y = 0;
                    r->cursor_y = r->scroll_y;  /* keep cursor in sync */
                } else {
                    reader_move_cursor((max_y - 3) / 2, 0);
                }
                break;

            case CTRL('u'):
                /* Half page up */
                if (r->preview_mode) {
                    int content_height = max_y - 3;
                    r->scroll_y -= content_height / 2;
                    if (r->scroll_y < 0) r->scroll_y = 0;
                    r->cursor_y = r->scroll_y;  /* keep cursor in sync */
                } else {
                    reader_move_cursor(-(max_y - 3) / 2, 0);
                }
                break;

            case 'w':
                /* Word forward - to start of next word */
                {
                    int y = r->cursor_y;
                    int x = r->cursor_x;
                    char *line = (y >= 0 && (size_t)y < r->line_count) ? r->lines[y] : NULL;
                    if (line) {
                        int len = strlen(line);
                        /* Skip current word if in one */
                        while (x < len && isalnum((unsigned char)line[x])) x++;
                        /* Skip whitespace/non-word */
                        while (x < len && !isalnum((unsigned char)line[x])) x++;
                        if (x >= len && (size_t)y + 1 < r->line_count) {
                            y++;
                            x = 0;
                            /* Skip leading whitespace on next line */
                            line = r->lines[y];
                            len = strlen(line);
                            while (x < len && !isalnum((unsigned char)line[x])) x++;
                        }
                        r->cursor_y = y;
                        r->cursor_x = x;
                        reader_ensure_cursor_visible(max_y, max_x);
                    }
                }
                break;

            case 'e':
                /* Word forward - to end of current/next word (vim-like) */
                {
                    int y = r->cursor_y;
                    int x = r->cursor_x;
                    char *line = (y >= 0 && (size_t)y < r->line_count) ? r->lines[y] : NULL;
                    if (line) {
                        int len = strlen(line);
                        /* If currently on a word char, first move past the current word */
                        if (x < len && isalnum((unsigned char)line[x])) {
                            while (x < len && isalnum((unsigned char)line[x])) x++;
                        }
                        /* Skip whitespace to get to next word */
                        while (x < len && !isalnum((unsigned char)line[x])) x++;
                        /* Now move to end of this word */
                        while (x < len && isalnum((unsigned char)line[x])) x++;
                        /* x is now at end of word (position after last char) */
                        /* Put cursor on last char of word */
                        if (x > 0) x--;
                        r->cursor_y = y;
                        r->cursor_x = x;
                        reader_ensure_cursor_visible(max_y, max_x);
                    }
                }
                break;

            case 'b':
                /* Word backward - to start of previous word */
                {
                    int y = r->cursor_y;
                    int x = r->cursor_x;
                    char *line = (y >= 0 && (size_t)y < r->line_count) ? r->lines[y] : NULL;
                    if (line) {
                        /* Skip whitespace before current position */
                        while (x > 0 && !isalnum((unsigned char)line[x - 1])) x--;
                        /* Skip current word */
                        while (x > 0 && isalnum((unsigned char)line[x - 1])) x--;
                        if (x <= 0 && y > 0) {
                            y--;
                            line = r->lines[y];
                            int len = strlen(line);
                            x = len;
                            /* Skip trailing whitespace */
                            while (x > 0 && !isalnum((unsigned char)line[x - 1])) x--;
                            /* Skip word */
                            while (x > 0 && isalnum((unsigned char)line[x - 1])) x--;
                        }
                        r->cursor_y = y;
                        r->cursor_x = x;
                        reader_ensure_cursor_visible(max_y, max_x);
                    }
                }
                break;

            case '?':
                /* Show reader help */
                endwin();
                printf("\n");
                printf("╔══════════════════════════════════════════════════════════════╗\n");
                printf("║              kbfs Reader - Vim-like Key Bindings             ║\n");
                printf("╠══════════════════════════════════════════════════════════════╣\n");
                printf("║  導航                                                        ║\n");
                printf("║    h, ←     游標左移                                         ║\n");
                printf("║    j, ↓     游標下移                                         ║\n");
                printf("║    k, ↑     游標上移                                         ║\n");
                printf("║    l, →     游標右移                                         ║\n");
                printf("║    0        跳到行首                                         ║\n");
                printf("║    $        跳到行尾                                         ║\n");
                printf("║    gg       跳到檔案開頭                                     ║\n");
                printf("║    G        跳到檔案結尾                                     ║\n");
                printf("║    w        跳到下一個單字開頭                               ║\n");
                printf("║    e        跳到下一個單字結尾                               ║\n");
                printf("║    b        跳到上一個單字開頭                               ║\n");
                printf("║    Space    向下翻頁 (Page Down)                             ║\n");
                printf("║    C-b      向上翻頁 (Page Up)                               ║\n");
                printf("║    C-d      向下半頁 (Half Page Down)                        ║\n");
                printf("║    C-u      向上半頁 (Half Page Up)                          ║\n");
                printf("║                                                              ║\n");
                printf("║  選取與複製                                                  ║\n");
                printf("║    v        進入/退出 Visual 模式                            ║\n");
                printf("║    y        複製選取內容到剪貼簿                             ║\n");
                printf("║                                                              ║\n");
                printf("║  模式切換                                                    ║\n");
                printf("║    i        切換 預覽/編輯 模式                             ║\n");
                printf("║                                                              ║\n");
                printf("║  搜尋                                                        ║\n");
                printf("║    /        搜尋                                             ║\n");
                printf("║    n        下一個結果                                       ║\n");
                printf("║    N        上一個結果                                       ║\n");
                printf("║                                                              ║\n");
                printf("║  其他                                                        ║\n");
                printf("║    q        退出 Reader 模式                                 ║\n");
                printf("║    ?        顯示此幫助畫面                                   ║\n");
                printf("╚══════════════════════════════════════════════════════════════╝\n");
                printf("\n按 Enter 繼續...");
                getchar();
                refresh_display();
                break;
        }
    }

    curs_set(0);
}

/* Initialize TUI - uses /dev/tty so it works even when stdin/stdout are piped
 * (e.g. inside Claude Code's ! command) */
int mdkb_tui_init(void) {
    setlocale(LC_ALL, "");

    /* Try /dev/tty first (works in piped environments like Claude Code) */
    g_tty_in = fopen("/dev/tty", "r");
    g_tty_out = fopen("/dev/tty", "w");

    if (g_tty_in && g_tty_out) {
        g_screen = newterm(NULL, g_tty_out, g_tty_in);
        if (!g_screen) {
            fclose(g_tty_in);
            fclose(g_tty_out);
            g_tty_in = NULL;
            g_tty_out = NULL;
            return -1;
        }
        set_term(g_screen);
        refresh();  /* flush terminal init sequences so ACS line-drawing works */
    } else {
        /* Fallback: direct terminal (normal invocation) */
        if (g_tty_in) fclose(g_tty_in);
        if (g_tty_out) fclose(g_tty_out);
        g_tty_in = NULL;
        g_tty_out = NULL;
        initscr();
    }

    set_escdelay(25);  /* Reduce ESC key delay from 1000ms default */
    cbreak();
    noecho();
    nonl();
    intrflush(stdscr, FALSE);
    keypad(stdscr, TRUE);
    curs_set(0);

    init_colors();

    return 0;
}

/* Cleanup TUI */
void mdkb_tui_cleanup(void) {
    endwin();
    if (g_screen) {
        delscreen(g_screen);
        g_screen = NULL;
    }
    if (g_tty_in) { fclose(g_tty_in); g_tty_in = NULL; }
    if (g_tty_out) { fclose(g_tty_out); g_tty_out = NULL; }
}

/* Run TUI main loop */
/* Load an index for a given root path (used for lazy-loading the alt mode) */
static KB_Index *load_index_for_root(const char *root) {
    KB_Index *idx = mdkb_index_new();
    mdkb_fs_scan(idx, root);
    mdkb_index_sort_by_time(idx);
    return idx;
}

/* Switch to a pre-loaded index (no disk I/O) */
static void tui_switch_to_index(KB_Index *new_index, const char *new_root) {
    /* Free old search and filter state */
    if (g_tui.search_query) { free(g_tui.search_query); g_tui.search_query = NULL; }
    if (g_tui.results) { mdmdkb_search_free(g_tui.results); g_tui.results = NULL; }
    if (g_tui.topic_filter) { free(g_tui.topic_filter); g_tui.topic_filter = NULL; }
    if (g_tui.type_filter) { free(g_tui.type_filter); g_tui.type_filter = NULL; }
    for (size_t i = 0; i < g_tui.tag_filter_count; i++) free(g_tui.tag_filters[i]);
    free(g_tui.tag_filters); g_tui.tag_filters = NULL; g_tui.tag_filter_count = 0;

    /* Switch index pointer (no freeing — old index stays cached) */
    g_tui.index = new_index;

    /* Update root path */
    free(g_tui.mdkb_root);
    g_tui.mdkb_root = kb_strdup(new_root);

    /* Reset selection */
    g_tui.selected = 0;
    g_tui.offset = 0;
    g_tui.top_offset = 0;

    /* Invalidate caches */
    invalidate_filter_cache();
}

int mdkb_tui_run(KB_Index *index, const char *mdkb_root) {
    if (!index) return -1;

    g_tui.index = index;
    g_tui.mdkb_root = mdkb_root ? kb_strdup(mdkb_root) : NULL;
    g_tui.selected = 0;
    g_tui.offset = 0;
    g_tui.top_offset = 0;
    g_tui.mode = MODE_LIST;
    g_tui.running = 1;
    g_tui.in_code_block = 0;
    g_tui.archive_mode = 0;  /* default to knowledge (fast startup) */
    g_tui.marked = calloc(index->entry_capacity, sizeof(bool));
    g_tui.mark_count = 0;
    g_tui.loaded_marks = calloc(index->entry_capacity, sizeof(bool));
    g_tui.loaded_mark_count = 0;
    memset(&g_tui.reader, 0, sizeof(g_tui.reader));

    /* Initialize dual-index cache */
    {
        const char *home = getenv("HOME");
        if (home) {
            g_tui.archive_root = kb_path_join(home, ".mdkb/archive");
            g_tui.knowledge_root = kb_path_join(home, ".mdkb/knowledge");
        }
    }
    /* The initial index is knowledge; archive is lazy-loaded on first Tab */
    g_tui.knowledge_index = index;
    g_tui.knowledge_marked = g_tui.marked;
    g_tui.knowledge_mark_count = 0;
    g_tui.knowledge_loaded_marks = g_tui.loaded_marks;
    g_tui.knowledge_loaded_mark_count = 0;
    g_tui.archive_index = NULL;  /* lazy-loaded on first Tab */
    g_tui.archive_marked = NULL;
    g_tui.archive_loaded_marks = NULL;

    /* Initialize filter cache */
    g_tui.filtered_indices = NULL;
    g_tui.filtered_count = 0;
    g_tui.filter_cache_valid = false;

    /* Initialize match line cache */
    g_tui.cached_match_entry_id = 0;
    g_tui.cached_match_query = NULL;
    g_tui.cached_match_lines = NULL;
    g_tui.cached_match_count = 0;

    /* Calculate left pane width (30% or min 30 chars) */
    int max_x, max_y;
    getmaxyx(stdscr, max_y, max_x);
    (void)max_y;
    g_tui.left_width = max_x * 0.3;
    if (g_tui.left_width < 30) g_tui.left_width = 30;
    if (g_tui.left_width > max_x / 2) g_tui.left_width = max_x / 2;

    /* Setup inotify for auto-refresh */
    inotify_setup(g_tui.mdkb_root);

    /* Main loop — timeout lets us poll inotify between keypresses */
    timeout(500);
    refresh_display();

    while (g_tui.running) {
        if (g_tui.mode == MODE_LIST) {
            int key = getch();
            if (key == ERR) {
                /* No keypress — check inotify for file changes */
                if (inotify_check()) {
                    tui_merge_new_entries();
                    invalidate_filter_cache();
                    refresh_display();
                }
            } else {
                handle_key(key);
                refresh_display();
            }
        }
    }

    /* Cleanup */
    inotify_cleanup();
    timeout(-1);  /* restore blocking getch */
    reader_cleanup();
    free(g_tui.mdkb_root);
    g_tui.mdkb_root = NULL;

    /* Free dual-index cache — marks are stored in archive/knowledge slots */
    /* Determine which marks are currently active vs stored in slots */
    if (g_tui.archive_mode) {
        /* Currently in archive mode: g_tui.marked points to archive_marked */
        free(g_tui.marked);  /* same as archive_marked */
        free(g_tui.loaded_marks);
        /* Free knowledge marks separately if they were allocated */
        if (g_tui.knowledge_marked) free(g_tui.knowledge_marked);
        if (g_tui.knowledge_loaded_marks) free(g_tui.knowledge_loaded_marks);
    } else {
        /* Currently in knowledge mode: g_tui.marked points to knowledge_marked */
        free(g_tui.marked);  /* same as knowledge_marked */
        free(g_tui.loaded_marks);
        /* Free archive marks separately */
        if (g_tui.archive_marked) free(g_tui.archive_marked);
        if (g_tui.archive_loaded_marks) free(g_tui.archive_loaded_marks);
    }
    g_tui.marked = NULL;
    g_tui.loaded_marks = NULL;
    g_tui.archive_marked = NULL;
    g_tui.knowledge_marked = NULL;
    g_tui.archive_loaded_marks = NULL;
    g_tui.knowledge_loaded_marks = NULL;

    /* Free the non-active cached index (active one is freed by caller via g_index) */
    if (g_tui.knowledge_index && g_tui.knowledge_index != g_tui.index) {
        mdkb_index_free(g_tui.knowledge_index);
    }
    if (g_tui.archive_index && g_tui.archive_index != g_tui.index) {
        mdkb_index_free(g_tui.archive_index);
    }
    g_tui.knowledge_index = NULL;
    g_tui.archive_index = NULL;

    free(g_tui.archive_root);
    g_tui.archive_root = NULL;
    free(g_tui.knowledge_root);
    g_tui.knowledge_root = NULL;

    /* Free filter cache */
    free(g_tui.filtered_indices);
    g_tui.filtered_indices = NULL;

    /* Free match line cache */
    free(g_tui.cached_match_lines);
    g_tui.cached_match_lines = NULL;
    free(g_tui.cached_match_query);
    g_tui.cached_match_query = NULL;

    free(g_tui.topic_filter);
    g_tui.topic_filter = NULL;
    free(g_tui.type_filter);
    g_tui.type_filter = NULL;
    for (size_t i = 0; i < g_tui.tag_filter_count; i++) free(g_tui.tag_filters[i]);
    free(g_tui.tag_filters); g_tui.tag_filters = NULL; g_tui.tag_filter_count = 0;
    free_mark_paths(g_tui.saved_mark_paths, g_tui.saved_mark_path_count);
    g_tui.saved_mark_paths = NULL;
    g_tui.saved_mark_path_count = 0;
    free_mark_paths(g_tui.saved_loaded_mark_paths, g_tui.saved_loaded_mark_path_count);
    g_tui.saved_loaded_mark_paths = NULL;
    g_tui.saved_loaded_mark_path_count = 0;

    /* If a file was picked (L key), print its path after TUI cleanup */
    if (g_tui.picked_path) {
        endwin();  /* ensure ncurses is done before stdout */
        printf("%s\n", g_tui.picked_path);
        fflush(stdout);
        free(g_tui.picked_path);
        g_tui.picked_path = NULL;
    }

    return 0;
}
