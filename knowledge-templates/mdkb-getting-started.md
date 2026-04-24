---
title: "mdkb — Getting Started Guide"
type: knowledge
repo: "mdkb"
topic: "getting-started"
tags: [mdkb, tutorial, getting-started, type:knowledge]
status: verified
sources:
  - session: "install"
    date: "2026-01-01"
updated: 0
---

# mdkb — Getting Started Guide

## What is mdkb?

mdkb is a local knowledge base CLI tool that integrates with Claude Code. It lets you:

- **Capture** knowledge from Claude Code conversations automatically (via sync hook)
- **Search** your knowledge base using BM25 full-text search
- **Load** notes into Claude Code as system prompt context (via the L key)
- **Read** loaded notes interactively using the `/mdkb-read` skill

The typical workflow: have a conversation with Claude → mdkb extracts the key knowledge → next time you open Claude, load the relevant note → Claude already knows the context.

---

## Installation

```bash
git clone <repo-url>
cd mdkb
./install.sh
```

This builds the binary, installs it to `/usr/local/bin/mdkb`, installs Claude Code skills, and initializes `~/.mdkb/`.

---

## CLI Commands

```bash
mdkb                        # Launch TUI (interactive browser)
mdkb -q "keyword"           # BM25 search, returns JSON
mdkb --load "keyword"       # Search and dump top result content
mdkb -q "keyword" -l 5      # Search, return top 5 results
mdkb --sync-on              # Enable auto-sync on session end
mdkb --sync-off             # Disable auto-sync
mdkb --reindex              # Rebuild search index
mdkb -i /path/to/session    # Manually ingest a conversation file
mdkb --archive              # Browse raw conversation archive
mdkb -v                     # Show version
mdkb -h                     # Show help
```

---

## TUI Key Bindings

Launch TUI with `mdkb`. Press `?` inside TUI to show the help screen.

### Navigation
| Key | Action |
|-----|--------|
| `j` / `↓` | Move selection down |
| `k` / `↑` | Move selection up |
| `g` | Jump to first entry |
| `G` | Jump to last entry |
| `Tab` | Toggle between Knowledge and Archive tabs |
| `q` / `Q` | Quit |

### Search & Filter
| Key | Action |
|-----|--------|
| `/` | Type a search keyword (BM25) |
| `r` / `R` | Clear search, show all |
| `t` | Filter by Topic (press again to clear) |
| `T` | Filter by Type: `knowledge` / `code` / `workflow` |
| `i` | Inbox Review — triage draft notes in `_inbox/` |

### Preview
| Key | Action |
|-----|--------|
| `Enter` | Open Markdown Reader for current entry |
| `Space` | Scroll preview down |
| `PgUp` | Scroll preview up |

### Multi-select
| Key | Action |
|-----|--------|
| `m` | Toggle mark on current entry, cursor moves down |
| `A` | Mark all visible entries (respects filter); press again to unmark all |
| `M` | Clear all marks |

### Actions
| Key | Action |
|-----|--------|
| `L` | **Launch Claude Code** with marked notes loaded as context. If no marks, launches with the current note. If a session is suspended, resumes it. |
| `R` | Resume a suspended Claude Code session |
| `E` | Edit title / topic / tags of the current note inline |
| `Y` | Yank (copy) the current note content to clipboard |
| `d` | Delete the current note (with confirmation) |
| `?` | Show help screen |

---

## Claude Code Skills

After installation, the following slash commands are available inside Claude Code:

| Skill | Trigger | What it does |
|-------|---------|--------------|
| `/mdkb-read` | After loading notes via L key | Guides you through loaded notes step by step |
| `/mdkb-workflow` | When a workflow note is loaded | Executes a loaded SOP/workflow note |
| `/mdkb-workflow-create` | When you want to save a new SOP | Interactive Q&A to create a new workflow note |
| `/mdkb-tidy` | Periodically | Merge duplicates, fix stale code refs, archive low-value notes |
| `/mdkb-sync-on` | Once, to enable auto-sync | Registers a Stop hook: syncs conversation on session end |
| `/mdkb-sync-off` | To pause sync | Removes the Stop hook |

---

## Typical Workflow

### First time: enable auto-sync

```
1. Run: mdkb --sync-on
   (or type /mdkb-sync-on inside Claude Code)
```

From then on, every Claude Code session is automatically ingested when you exit. Knowledge notes appear in `~/.mdkb/knowledge/`.

### Loading notes into Claude

```
1. Run: mdkb
2. Use / to search for a topic
3. Press m to mark relevant notes (or A to mark all visible)
4. Press L — Claude Code launches with notes loaded as system prompt context
5. Inside Claude, type: /mdkb-read
```

Multi-select tip: use `t` to filter by topic first, then `A` to mark all visible notes at once, then `L`. This loads an entire topic's worth of context in one step.

### Suspend and resume Claude (Ctrl+Z / R)

mdkb and Claude Code are designed to work together in a loop — you can freely switch between them without losing session state:

```
1. In mdkb TUI, press L → Claude Code launches (mdkb goes to background)
2. In Claude, do your work
3. Press Ctrl+Z → Claude suspends, you return to mdkb TUI
4. Browse, search, or mark more notes in mdkb
5. Press R → resumes the suspended Claude session
   (or press L again with new marks → Claude resumes with additional notes injected)
6. Repeat as needed
```

This lets you look up notes mid-conversation without losing Claude's context. The suspended session is fully preserved — conversation history, tool state, everything.

### Reviewing extracted notes

```
1. Run: mdkb
2. Press i to open Inbox Review
3. Review draft notes — accept, edit tags/topic, or discard
```

### Periodic cleanup

```
Inside Claude Code:
/mdkb-tidy              # interactive scope selection
/mdkb-tidy mdkb-tui    # directly target a topic
```

---

## Directory Structure

```
~/.mdkb/
├── knowledge/          # organized notes (repo/topic/*.md)
│   ├── _inbox/         # draft notes awaiting triage
│   └── _archive/       # archived notes (hidden from main index)
└── archive/            # raw conversation transcripts (Tab key in TUI)
```

Knowledge notes use YAML frontmatter:

```yaml
---
title: "Note Title"
type: knowledge       # knowledge | code | workflow
repo: "my-project"
topic: "auth"
tags: [jwt, oauth, type:knowledge]
status: verified
updated: 1745000000   # unix timestamp
---
```

---

## Tips

- **BM25 weights**: title matches score 10×, tag matches 5×, content 1×. Use specific words in titles for better search.
- **Multi-note loading**: mark several notes with `m` then press `L` — all are injected as separate system prompt sections, grouped by topic.
- **Topic batch-select**: filter by topic with `t`, then press `A` to mark all visible at once, then `L` — loads an entire topic in one step.
- **Ctrl+Z loop**: press Ctrl+Z inside Claude to suspend it and return to mdkb TUI; press `R` to resume. Use this to look up notes mid-conversation without losing Claude's context.
- **Code notes**: notes tagged `type:code` contain file paths and line numbers. Use `/mdkb-tidy` to detect stale references when code moves.
- **Workflow notes**: tagged `type:workflow`, executable via `/mdkb-workflow`. Great for SOPs you repeat across sessions.
