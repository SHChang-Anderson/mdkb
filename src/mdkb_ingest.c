/* timegm is a GNU/BSD extension, must come before any #include */
#define _GNU_SOURCE

/*
 * mdkb - Markdown Knowledge Base
 * Claude Code conversation import (JSONL format)
 *
 * Reads ~/.claude/projects/<project>/<session-id>.jsonl
 * Converts to readable Markdown in output_dir/YYYY/MM/
 * Resume link: claude --resume <session-id>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include "mdkb.h"

/* ============================================================================
 * Minimal JSON helpers (single-line JSONL scope)
 * ============================================================================ */

/* Extract string value for key from a JSON line (newly allocated).
 * Handles backslash escapes.  Returns NULL if not found. */
static char *jstr(const char *line, const char *key) {
    char pattern[256];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);

    const char *p = strstr(line, pattern);
    if (!p) return NULL;
    p += strlen(pattern);

    size_t cap = 4096;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    size_t len = 0;

    while (*p && *p != '"') {
        if (*p == '\\' && *(p + 1)) {
            p++;
            char ch = *p;
            if      (ch == 'n') ch = '\n';
            else if (ch == 't') ch = '\t';
            else if (ch == 'r') ch = '\r';
            if (len + 2 >= cap) { cap *= 2; buf = realloc(buf, cap); if (!buf) return NULL; }
            buf[len++] = ch;
        } else {
            if (len + 2 >= cap) { cap *= 2; buf = realloc(buf, cap); if (!buf) return NULL; }
            buf[len++] = *p;
        }
        p++;
    }
    buf[len] = '\0';
    return buf;
}

/* Return 1 and set *out if "key":true/false found, else 0 */
static int jbool(const char *line, const char *key, int *out) {
    char pattern[256];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char *p = strstr(line, pattern);
    if (!p) return 0;
    p += strlen(pattern);
    while (*p == ' ') p++;
    if (strncmp(p, "true",  4) == 0) { *out = 1; return 1; }
    if (strncmp(p, "false", 5) == 0) { *out = 0; return 1; }
    return 0;
}

/* ============================================================================
 * Content extraction from a JSONL message line
 * ============================================================================ */

/* Extract readable text from the "content" field of one JSONL line.
 * content can be a plain string or an array of typed blocks.
 * Returns newly-allocated string, or NULL if nothing useful. */
static char *extract_content(const char *line, int is_assistant) {
    /* Locate the "message" object */
    const char *base = strstr(line, "\"message\":{");
    if (!base) base = line;

    /* --- Plain string content --- */
    {
        const char *tag = "\"content\":\"";
        const char *p = strstr(base, tag);
        if (p) {
            char *s = jstr(base, "content");
            if (s && strlen(s) > 0 &&
                !strstr(s, "<local-command") &&
                !strstr(s, "<command-name>") &&
                !strstr(s, "<system-reminder>")) {
                return s;
            }
            free(s);
            return NULL;
        }
    }

    /* --- Array content --- */
    const char *arr = strstr(base, "\"content\":[");
    if (!arr) return NULL;
    arr += strlen("\"content\":[");

    size_t cap = 8192;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    buf[0] = '\0';
    size_t len = 0;
    int first = 1;

    const char *p = arr;
    while (*p && *p != ']') {
        /* Advance to next object */
        while (*p && *p != '{' && *p != ']') p++;
        if (*p != '{') break;

        /* Find matching closing brace, skipping string contents so that
         * { or } inside string values don't corrupt the depth counter. */
        const char *obj_s = p;
        int depth = 0;
        const char *obj_e = p;
        while (*obj_e) {
            if (*obj_e == '"') {
                /* Skip over the entire JSON string, respecting backslash escapes */
                obj_e++;
                while (*obj_e && *obj_e != '"') {
                    if (*obj_e == '\\') obj_e++;  /* skip escaped character */
                    if (*obj_e) obj_e++;
                }
                if (*obj_e == '"') obj_e++;
                continue;
            }
            if      (*obj_e == '{') depth++;
            else if (*obj_e == '}') { depth--; if (depth == 0) { obj_e++; break; } }
            obj_e++;
        }

        size_t olen = (size_t)(obj_e - obj_s);
        char *obj = malloc(olen + 1);
        if (!obj) { p = obj_e; continue; }
        memcpy(obj, obj_s, olen);
        obj[olen] = '\0';

        char *type = jstr(obj, "type");

        if (type && strcmp(type, "text") == 0) {
            char *text = jstr(obj, "text");
            /* Skip injected system/command blocks in array content */
            if (text && (strstr(text, "<local-command") ||
                         strstr(text, "<command-name>") ||
                         strstr(text, "<command-message>") ||
                         strstr(text, "<system-reminder>") ||
                         strstr(text, "<local-command-caveat>"))) {
                free(text);
                text = NULL;
            }
            if (text && strlen(text) > 0) {
                if (!first) {
                    if (len + 2 >= cap) { cap *= 2; buf = realloc(buf, cap); }
                    buf[len++] = '\n';
                    buf[len] = '\0';
                }
                size_t tl = strlen(text);
                while (len + tl + 2 >= cap) { cap *= 2; buf = realloc(buf, cap); }
                memcpy(buf + len, text, tl);
                len += tl;
                buf[len] = '\0';
                first = 0;
            }
            free(text);
        } else if (type && strcmp(type, "tool_use") == 0 && is_assistant) {
            /* Show tool calls as blockquotes (only if they have a description) */
            char *name  = jstr(obj, "name");
            char *desc  = jstr(obj, "description");
            if (!desc) desc = jstr(obj, "command");

            /* Skip tool calls without descriptions - they add noise */
            if (desc && strlen(desc) > 0) {
                char tool_line[512];
                snprintf(tool_line, sizeof(tool_line), "\n> **[%s]** %s\n",
                         name ? name : "tool", desc);

                size_t tl = strlen(tool_line);
                while (len + tl + 2 >= cap) { cap *= 2; buf = realloc(buf, cap); }
                memcpy(buf + len, tool_line, tl);
                len += tl;
                buf[len] = '\0';
                first = 0;
            }
            free(name);
            free(desc);
        }
        /* thinking / tool_result: skip */

        free(type);
        free(obj);
        p = obj_e;
    }

    if (len == 0) { free(buf); return NULL; }
    return buf;
}

/* ============================================================================
 * JSONL parsing
 * ============================================================================ */

typedef enum { ROLE_SKIP, ROLE_USER, ROLE_ASSISTANT } MsgRole;

typedef struct {
    MsgRole  role;
    char    *text;
    char    *timestamp;
    char    *slug;
    char    *session_id;
    char    *cwd;
} Msg;

static void msg_free(Msg *m) {
    free(m->text);
    free(m->timestamp);
    free(m->slug);
    free(m->session_id);
    free(m->cwd);
    memset(m, 0, sizeof(*m));
}

static Msg parse_line(const char *line) {
    Msg m = {0};
    m.role = ROLE_SKIP;

    /* The top-level "type" field may come AFTER the nested "message" object
     * (whose content blocks also have "type" fields), so jstr() would find the
     * wrong value.  Use a direct substring match instead — "user" and "assistant"
     * never appear as content-block type values, so this is unambiguous. */
    int is_user      = (strstr(line, "\"type\":\"user\"")      != NULL);
    int is_assistant = (strstr(line, "\"type\":\"assistant\"") != NULL);
    if (!is_user && !is_assistant) return m;
    if (is_user && is_assistant)   return m;  /* malformed, skip */

    int is_meta = 0, is_side = 0;
    jbool(line, "isMeta",      &is_meta);
    jbool(line, "isSidechain", &is_side);
    if (is_meta || is_side) return m;

    /* Skip synthetic (local command) assistant messages */
    if (is_assistant && strstr(line, "\"model\":\"<synthetic>\"")) return m;

    /* Skip tool-result user messages (system-generated responses to tool calls) */
    if (is_user && strstr(line, "\"tool_use_id\"")) return m;

    m.role       = is_user ? ROLE_USER : ROLE_ASSISTANT;
    m.text       = extract_content(line, is_assistant);
    m.timestamp  = jstr(line, "timestamp");
    m.slug       = jstr(line, "slug");
    m.session_id = jstr(line, "sessionId");
    m.cwd        = jstr(line, "cwd");

    if (!m.text || strlen(m.text) == 0) {
        msg_free(&m);
        m.role = ROLE_SKIP;
    }
    return m;
}

/* ============================================================================
 * Helpers
 * ============================================================================ */

/* Equivalent of mkdir -p: create all parent directories */
static void mkdir_p(const char *path) {
    char tmp[4096];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

static char *safe_filename(const char *s) {
    if (!s || !*s) return kb_strdup("untitled");
    size_t len = strlen(s);
    char *out = malloc(len + 1);
    if (!out) return kb_strdup("untitled");
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (isalnum(c) || c == '-' || c == '_') out[j++] = (char)c;
        else if (c == ' ')                       out[j++] = '-';
    }
    if (j == 0) { free(out); return kb_strdup("untitled"); }
    out[j] = '\0';
    return out;
}

static time_t parse_iso(const char *ts) {
    if (!ts) return 0;
    struct tm tm = {0};
    sscanf(ts, "%d-%d-%dT%d:%d:%d",
           &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
           &tm.tm_hour, &tm.tm_min, &tm.tm_sec);
    tm.tm_year -= 1900;
    tm.tm_mon  -= 1;
    return timegm(&tm);
}

/* ============================================================================
 * Core: JSONL -> Markdown
 * ============================================================================ */

static int jsonl_to_markdown(const char *jsonl_path, const char *output_dir) {
    FILE *fp = fopen(jsonl_path, "r");
    if (!fp) return -1;

    /* Session ID from filename */
    const char *fname = strrchr(jsonl_path, '/');
    fname = fname ? fname + 1 : jsonl_path;
    char session_id[256] = {0};
    strncpy(session_id, fname, sizeof(session_id) - 1);
    char *dot = strstr(session_id, ".jsonl");
    if (dot) *dot = '\0';

    /* Collect messages */
    size_t cap = 64, count = 0;
    Msg *msgs = malloc(cap * sizeof(Msg));
    if (!msgs) { fclose(fp); return -1; }

    char *slug = NULL, *cwd = NULL;
    time_t first_ts = 0;
    char *title = NULL;

    char line[65536];
    while (fgets(line, sizeof(line), fp)) {
        Msg m = parse_line(line);
        if (m.role == ROLE_SKIP) continue;

        if (!slug && m.slug)       slug = kb_strdup(m.slug);
        if (!cwd  && m.cwd)        cwd  = kb_strdup(m.cwd);
        if (first_ts == 0 && m.timestamp) first_ts = parse_iso(m.timestamp);

        /* Use first human turn as title (cleaned up) */
        if (!title && m.role == ROLE_USER && m.text) {
            /* Skip code-heavy messages: find first non-code line */
            const char *src = m.text;
            while (*src == ' ' || *src == '\n' || *src == '\r' || *src == '\t') src++;
            /* Skip code fences */
            if (strncmp(src, "```", 3) == 0) src = NULL;

            if (src && strlen(src) > 0) {
                size_t mx = 60;
                title = malloc(mx + 4);
                /* Copy up to first newline or mx chars, collapsing whitespace */
                size_t j = 0;
                int prev_space = 0;
                for (size_t k = 0; src[k] && j < mx; k++) {
                    if (src[k] == '\n' || src[k] == '\r') {
                        if (j > 0) break;  /* stop at first newline */
                        continue;
                    }
                    if (src[k] == ' ' || src[k] == '\t') {
                        if (!prev_space && j > 0) { title[j++] = ' '; }
                        prev_space = 1;
                        continue;
                    }
                    prev_space = 0;
                    title[j++] = src[k];
                }
                /* Trim trailing spaces */
                while (j > 0 && title[j - 1] == ' ') j--;
                if (j > 0 && j < mx) {
                    title[j] = '\0';
                } else if (j >= mx) {
                    title[mx] = '\0';
                    /* Find last space for clean truncation */
                    char *sp = strrchr(title, ' ');
                    if (sp && sp > title + mx / 2) {
                        strcpy(sp, "...");
                    } else {
                        /* Back up to UTF-8 character boundary before appending "..." */
                        size_t cut = mx - 3;
                        while (cut > 0 && (title[cut] & 0xC0) == 0x80)
                            cut--;
                        strcpy(title + cut, "...");
                    }
                } else {
                    free(title);
                    title = NULL;
                }
            }
        }

        if (count >= cap) { cap *= 2; msgs = realloc(msgs, cap * sizeof(Msg)); }
        msgs[count++] = m;
    }
    fclose(fp);

    if (count == 0) { free(msgs); free(slug); free(cwd); free(title); return 0; }

    /* Title fallback: use slug if title is missing or too short */
    if (title && strlen(title) < 3) { free(title); title = NULL; }
    if (!title && slug) {
        /* Convert slug from kebab-case to readable form */
        title = kb_strdup(slug);
        for (char *r = title; *r; r++)
            if (*r == '-') *r = ' ';
    }

    /* Build output path: output_dir/YYYY/MM/DATE_slug_sessionid.md */
    if (first_ts == 0) first_ts = time(NULL);
    struct tm *tm = localtime(&first_ts);

    char date_s[11], year_s[5], month_s[3];
    strftime(date_s,  sizeof(date_s),  "%Y-%m-%d", tm);
    strftime(year_s,  sizeof(year_s),  "%Y",       tm);
    strftime(month_s, sizeof(month_s), "%m",       tm);

    char *year_dir  = kb_path_join(output_dir, year_s);
    char *month_dir = kb_path_join(year_dir, month_s);
    mkdir_p(month_dir);

    char *slug_safe = safe_filename(slug ? slug : session_id);
    char md_fname[512];
    snprintf(md_fname, sizeof(md_fname), "%s_%s_%s.md",
             date_s, slug_safe, session_id);
    char *md_path = kb_path_join(month_dir, md_fname);

    /* Skip if Markdown is already newer than source */
    struct stat src_st, dst_st;
    if (stat(jsonl_path, &src_st) == 0 && stat(md_path, &dst_st) == 0 &&
        dst_st.st_mtime >= src_st.st_mtime) {
        goto cleanup;
    }

    /* Write Markdown */
    {
        FILE *out = fopen(md_path, "w");
        if (!out) { fprintf(stderr, "[ingest] Cannot write: %s\n", md_path); goto cleanup; }

        const char *ttl = title ? title : "Claude Conversation";

        /* Escape quotes for YAML */
        char safe_ttl[512];
        { size_t si = 0, di = 0;
          while (ttl[si] && di < sizeof(safe_ttl) - 2) {
              if (ttl[si] == '"') safe_ttl[di++] = '\\';
              safe_ttl[di++] = ttl[si++];
          }
          safe_ttl[di] = '\0'; }

        fprintf(out, "---\n");
        fprintf(out, "title: \"%s\"\n", safe_ttl);
        fprintf(out, "type: conversation\n");
        fprintf(out, "timestamp: %ld\n", (long)first_ts);
        fprintf(out, "session_id: \"%s\"\n", session_id);
        if (slug) fprintf(out, "slug: \"%s\"\n", slug);
        if (cwd)  fprintf(out, "cwd: \"%s\"\n",  cwd);
        fprintf(out, "tags: [claude, conversation]\n");
        fprintf(out, "source: \"%s\"\n", jsonl_path);
        fprintf(out, "---\n\n");

        fprintf(out, "# %s\n\n", ttl);
        fprintf(out, "*%s", date_s);
        if (cwd) fprintf(out, " \xc2\xb7 `%s`", cwd);
        fprintf(out, "*\n\n");
        fprintf(out, "> **Resume:** `claude --resume %s`\n\n", session_id);
        fprintf(out, "---\n\n");

        MsgRole prev_role = ROLE_SKIP;
        for (size_t i = 0; i < count; i++) {
            if (msgs[i].role == prev_role) {
                /* Merge consecutive same-role messages */
                fprintf(out, "%s\n\n", msgs[i].text);
            } else {
                /* New speaker: add separator (except before first turn) */
                if (prev_role != ROLE_SKIP)
                    fprintf(out, "---\n\n");
                if (msgs[i].role == ROLE_USER)
                    fprintf(out, "## Human\n\n%s\n\n", msgs[i].text);
                else
                    fprintf(out, "## Claude\n\n%s\n\n", msgs[i].text);
                prev_role = msgs[i].role;
            }
        }

        fclose(out);
        fprintf(stderr, "[ingest] %s\n", md_path);
    }

cleanup:
    for (size_t i = 0; i < count; i++) msg_free(&msgs[i]);
    free(msgs);
    free(slug); free(cwd); free(title);
    free(slug_safe); free(year_dir); free(month_dir); free(md_path);
    return 0;
}

/* ============================================================================
 * Directory scan
 * ============================================================================ */

static int scan_dir(const char *dir, const char *output_dir) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    int n = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char *full = kb_path_join(dir, ent->d_name);
        struct stat st;
        if (stat(full, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                n += scan_dir(full, output_dir);
            } else if (S_ISREG(st.st_mode)) {
                size_t nl = strlen(ent->d_name);
                if (nl > 6 && strcmp(ent->d_name + nl - 6, ".jsonl") == 0) {
                    if (jsonl_to_markdown(full, output_dir) == 0) n++;
                }
            }
        }
        free(full);
    }
    closedir(d);
    return n;
}

/* ============================================================================
 * Public API
 * ============================================================================ */

int mdkb_ingest_claude(KB_Index *index, const char *src_path, const char *output_dir) {
    (void)index;
    if (!src_path || !output_dir) return -1;

    char *src = kb_expand_tilde(src_path);
    char *out = kb_expand_tilde(output_dir);
    if (!src || !out) { free(src); free(out); return -1; }

    int ret = 0;
    struct stat st;
    if (stat(src, &st) != 0) {
        fprintf(stderr, "[ingest] Cannot access: %s\n", src);
        ret = -1;
    } else if (S_ISREG(st.st_mode)) {
        ret = jsonl_to_markdown(src, out);
    } else if (S_ISDIR(st.st_mode)) {
        int n = scan_dir(src, out);
        fprintf(stderr, "[ingest] Processed %d sessions\n", n);
    }

    free(src);
    free(out);
    return ret;
}
