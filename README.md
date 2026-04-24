# mdkb — Markdown Knowledge Base

A fast, vim-like TUI for capturing and retrieving knowledge from Claude Code conversations.

mdkb integrates with Claude Code: conversations are automatically extracted into structured Markdown notes, searchable via BM25, and loadable back into Claude as context with a single keypress.

## Features

- **BM25 full-text search** with title/tag/content weighting
- **Dual-pane TUI** — browse, preview, and read Markdown notes
- **Claude Code integration** — press `L` to load notes into a new Claude session
- **Auto-sync** — conversations are extracted into knowledge notes on session end
- **Claude Code skills** — `/mdkb-read`, `/mdkb-tidy`, `/mdkb-workflow`, and more

## Dependencies

```bash
# Debian/Ubuntu
sudo apt install libncursesw5-dev xclip

# macOS
brew install ncurses
```

Python 3 is required for the sync scripts.

## Installation

```bash
git clone <repo-url>
cd mdkb
./install.sh
```

`install.sh` will:
1. Build the binary
2. Install `mdkb` to `/usr/local/bin/`
3. Install Claude Code skills to `~/.claude/skills/`
4. Initialize `~/.mdkb/` directory structure
5. Write a getting-started note to your knowledge base

To uninstall:

```bash
./uninstall.sh          # keeps ~/.mdkb/ (your notes)
PURGE=1 ./uninstall.sh  # also deletes ~/.mdkb/
```

## Quick Start

```bash
# Enable auto-sync (once — registers a hook on Claude session end)
mdkb --sync-on

# Open the knowledge browser
mdkb

# In TUI: press L to load a note into Claude Code
# In Claude Code: run /mdkb-read for a guided walkthrough
```

Press `?` inside the TUI for the full key binding reference.

## CLI Reference

```
mdkb                        # Launch TUI
mdkb -q "keyword"           # BM25 search (JSON output)
mdkb --load "keyword"       # Search and print top result
mdkb -q "keyword" -l 5      # Return top 5 results
mdkb --sync-on              # Enable auto-sync hook
mdkb --sync-off             # Disable auto-sync hook
mdkb --reindex              # Rebuild search index
mdkb -i /path/to/session    # Manually ingest a conversation file
mdkb --archive              # Browse raw conversation archive
mdkb -v                     # Show version
```

## TUI Key Bindings

| Key | Action |
|-----|--------|
| `j` / `↓` | Move down |
| `k` / `↑` | Move up |
| `g` / `G` | Jump to first / last |
| `Tab` | Toggle Knowledge ↔ Archive tab |
| `/` | Search |
| `r` / `R` | Clear search |
| `t` | Filter by Topic |
| `T` | Filter by Type (knowledge / code / workflow) |
| `i` | Inbox Review — triage draft notes |
| `Enter` | Open Markdown Reader |
| `m` | Mark / unmark entry |
| `A` | Mark all visible entries (toggle) |
| `M` | Clear all marks |
| `L` | Launch Claude Code with marked notes as context |
| `R` | Resume suspended Claude session |
| `E` | Edit title / topic / tags inline |
| `Y` | Yank note content to clipboard |
| `d` | Delete note |
| `?` | Show help |

## Claude Code Skills

After installation the following slash commands are available inside Claude Code:

| Skill | Description |
|-------|-------------|
| `/mdkb-read` | Step-by-step guided reading of loaded notes |
| `/mdkb-workflow` | Execute a loaded workflow/SOP note |
| `/mdkb-workflow-create` | Interactive builder for new workflow notes |
| `/mdkb-tidy` | Merge duplicates, fix stale code refs, archive low-value notes |
| `/mdkb-sync-on` | Enable auto-sync hook |
| `/mdkb-sync-off` | Disable auto-sync hook |

## Knowledge Base Structure

```
~/.mdkb/
├── knowledge/           # Organized notes (repo/topic/*.md)
│   ├── _inbox/          # Draft notes awaiting triage
│   └── _archive/        # Archived notes
└── archive/             # Raw conversation transcripts
```

Notes use YAML frontmatter:

```yaml
---
title: "Note Title"
type: knowledge       # knowledge | code | workflow
repo: "my-project"
topic: "auth"
tags: [jwt, oauth, type:knowledge]
status: verified
updated: 1745000000
---
```

## Building from Source

```bash
make          # development build
make debug    # with debug symbols
make release  # optimized build
make clean    # remove build artifacts
```

