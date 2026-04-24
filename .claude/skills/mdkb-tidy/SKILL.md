---
name: mdkb-tidy
description: "Consolidate, deduplicate, and clean up mdkb knowledge notes. Use when: '/mdkb-tidy', 'tidy notes', 'clean up knowledge', 'consolidate notes', 'merge notes'. Supports scope filter: '/mdkb-tidy mdkb-tui', '/mdkb-tidy myproject/auth'."
version: "1.0"
author: "Anderson Chang"
metadata:
  category: "utility"
---

# mdkb-tidy — Knowledge Note Consolidation & Cleanup

Consolidate, deduplicate, and clean up mdkb knowledge notes with user-directed scope.

## Usage

```
/mdkb-tidy                     # interactive — ask user to choose scope first
/mdkb-tidy mdkb-tui            # directly target topic "mdkb-tui"
/mdkb-tidy myproject           # directly target repo "myproject"
/mdkb-tidy myproject/auth      # directly target repo "myproject", topic "auth"
```

## Knowledge directory

```
~/.mdkb/knowledge/
  ├── _inbox/          # draft notes awaiting triage
  ├── _archive/        # archived notes (from previous tidy runs)
  ├── <repo>/<topic>/  # organized notes
  └── ...
```

## Pre-flight: Pause mdkb-sync

Before any tidy work, suspend the sync hook to prevent it from ingesting tidy-session transcript as new notes.

1. Check if `~/.mdkb/.sync-enabled` exists — record the original state
2. Run `mdkb --sync-off 2>/dev/null` to disable sync
3. At the end of tidy (after Phase 5, or on early exit/skip), restore:
   - If sync was originally ON → run `mdkb --sync-on 2>/dev/null`
   - If sync was originally OFF → do nothing

**IMPORTANT:** Always restore sync state, even if the user skips all actions or aborts mid-tidy.

## Phase 0: Choose scope (ALWAYS DO THIS FIRST)

**If the user provided a scope argument** (e.g. `/mdkb-tidy mdkb-tui`), use it directly — skip to Phase 1.

**If no argument was given**, you MUST ask the user before scanning. Do the following:

1. List all available `<repo>/<topic>/` directories under `~/.mdkb/knowledge/` (excluding `_inbox/`, `_archive/`), with note counts:
   ```
   Available scopes:
   - myproject/auth    (12 notes)
   - myproject/api     (8 notes)
   - oss/core          (3 notes)
   - mdkb/mdkb-tui    (5 notes)
   - all               (28 notes total)
   ```

2. Ask the user which scope to tidy. Accept:
   - A topic name: `mdkb-tui`
   - A repo name: `myproject`
   - A repo/topic path: `myproject/auth`
   - Comma-separated: `mdkb-tui,mdkb`
   - `all` for everything

**Do NOT start scanning until the user has chosen a scope.**

## Phase 1: Scan

Read `.md` files matching the chosen scope (excluding `_inbox/` and `_archive/`).
For each note, extract from frontmatter:
- `title`, `repo`, `topic`, `type`, `tags`, `status`, `updated` timestamp
- `sources[].date` (when the note was created/updated)

Build an in-memory inventory. Do NOT read full content yet — only frontmatter.

Report:
```
Scanned N notes in "<scope>" (M topics).
```

## Phase 2: Analyze

Run these checks and collect findings:

### 2a. Duplicates & Overlaps
- Group notes by `repo` + `topic`
- Within each group, compare titles for similarity (shared keywords, version suffixes like v1/v2/v3)
- Flag groups where multiple notes cover the same subject

### 2b. Evolution Chains
- Detect version sequences: titles containing "v1", "v2", "v3", "part 1", "part 2", or notes with identical base titles + different suffixes
- These are candidates for merging into a single consolidated note

### 2c. Stale Code Notes
- For notes with `type:code` or tag `type:code`:
  - Read the note content
  - Extract file paths and line numbers referenced in the note
  - Use the Read tool to check if those files/lines still exist and match
  - Flag notes where code references are broken (file moved, function renamed, line numbers shifted significantly)

### 2d. Low-Value Notes
- Notes with very short content (< 100 words excluding frontmatter)
- Notes whose content is entirely subsumed by another note on the same topic
- Notes with no meaningful technical content (just "ok" or session metadata)

### 2e. Fragmented Topics
- Topics with 5+ notes that could potentially be consolidated
- Multiple small notes that together form one coherent document

## Phase 3: Report

Present findings as a numbered action list. Do NOT execute anything yet.

```
## Tidy Report

### Merge candidates (N)
1. [merge] myproject/auth/login-flow-v1.md + login-flow-v2.md + login-flow-v3.md
   → Keep v3 as base, fold unique content from v1/v2, archive originals
2. [merge] myproject/auth/session-bug.md + token-expiry-bug.md
   → Same root cause, combine into one note

### Stale code references (N)
3. [stale] myproject/auth/middleware.md
   → Line 42: `/home/user/project/src/auth.c:85` — function moved to line 112
   → Line 58: `/home/user/project/src/server.c:200` — function renamed

### Low-value notes (N)
4. [delete] myproject/auth/trivial-fix.md — 45 words, content covered by other notes

### Suggested action for each:
- merge: combine into one note, archive originals
- update: fix stale references to match current code
- archive: move to _archive/ (keep but hide from index)
- delete: remove entirely
- skip: leave as-is
```

## Phase 4: Execute

Ask the user: "Which actions to execute? (e.g. `1,2,4` or `all` or `skip`)"

For each approved action:

### Merge
Merge means **condense**, NOT concatenate. The word limit per note is **1200 words**.

1. Read all source notes fully
2. Identify overlapping content — keep the most accurate/recent version, discard redundant sections
3. Write a NEW condensed note that captures the final state of knowledge:
   - For evolution chains (v1→v2→v3): keep only the final design + key decisions from earlier versions
   - For overlapping bug notes: combine into one root-cause + fix note
   - Drop intermediate debugging steps, superseded approaches, and "ok" replies
4. The condensed note MUST stay under **1200 words** (excluding frontmatter). If it would exceed this:
   - Split at `## ` heading boundaries into multiple focused notes (same as categorize.py split logic)
   - Each split note gets its own clear title
5. Update frontmatter: merge tags, update timestamp, add all source sessions
6. Move original files to `~/.mdkb/knowledge/_archive/` (do NOT delete)
7. Show the condensed result for user confirmation before writing

**What to keep**: final conclusions, working code references, architecture decisions, gotchas
**What to drop**: trial-and-error debugging steps, superseded designs, conversation filler

### Update stale references
1. Read the note and the current source files
2. Find the correct current line numbers for referenced functions/code
3. Update the note's file paths and line numbers
4. Show diff for user confirmation

### Archive
1. Move to `~/.mdkb/knowledge/_archive/<repo>/<topic>/`
2. Add `status: archived` to frontmatter

### Delete
1. Show the note content one more time
2. Confirm with user
3. `unlink()` the file

## Phase 5: Summary & Restore Sync

```
Tidy complete:
- Merged: 3 sets → 3 notes
- Updated: 2 stale references
- Archived: 1 note
- Deleted: 1 note
- Skipped: 2
Total: N notes → M notes (reduced by K)
```

**After printing the summary**, restore sync to its original state (see Pre-flight step 3).

## Rules

- Respond in the user's language (follow CLAUDE.md preferences)
- NEVER auto-execute — always show the report first and wait for user selection
- For merges, always show the merged content before writing
- Archived notes go to `_archive/`, not deleted — reversible by default
- When checking stale code references, use Grep to find where functions moved to (don't just report "not found")
- If a note has a `session_id`, preserve it in the merged note's `sources` list
- Keep the most specific/detailed content when merging; drop generic summaries
