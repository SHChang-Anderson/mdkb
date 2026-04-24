---
name: mdkb-read
description: "Guided reading of loaded knowledge document. Use when: '/mdkb-read', 'walk me through', 'explain loaded context', 'guide the knowledge'."
version: "1.0"
author: "Anderson Chang"
metadata:
  category: "utility"
---

# mdkb-read — Guided Knowledge Reading

Knowledge files are listed in your system prompt as file paths with titles. The system prompt contains the INDEX only (paths + titles), NOT the full content.

**You MUST use the Read tool to read the actual file content from the paths listed in the system prompt before discussing any concepts.**

Look for entries in the system prompt that match this pattern:
```
The following knowledge files are loaded. Use the Read tool to access full content when needed:
1. /path/to/file.md — Title
2. /path/to/file2.md — Title 2
```

If no knowledge file paths are found in the system prompt, tell the user: "No knowledge files loaded. Use the L key in mdkb TUI to load notes first."

## Step 0: Read the files

Before anything else, use the Read tool to read ALL knowledge files listed in the system prompt. You need the full content to provide accurate guided reading.

## Step 1: Overview

Give a brief overview (3 lines max): title, date, main topic. Then list key concepts as a short numbered list:

```
Key concepts:
1. Concept — brief phrase
2. Concept — brief phrase
3. Concept — brief phrase
```

Keep descriptions to a few words each. No preamble, no commentary. List 3-7 items.

## Step 2: User chooses

Let the user decide what to discuss:
- Specific numbers (e.g. `1, 3, 5`) — only discuss those
- `all` — discuss everything, but one at a time
- `skip` — skip this section entirely

## Step 3: Discuss one concept at a time

**One concept per response. Never combine multiple concepts.**

For each concept:
- Explain clearly with enough context to understand why it matters.
- If the concept involves code, show relevant snippets with file paths.
- Don't over-explain. The user will ask if they want to go deeper.
- **Stay on the current concept** until the user says "next" / "ok" / "繼續". Answer follow-up questions without jumping ahead.

## Step 4: Pacing

- After finishing one concept, ask if ready for the next.
- **After 3-4 concepts, check in**: "Continue or pause for now?"
- If a concept seems irrelevant or trivial, say so briefly and offer to skip.
- The user controls the pace. Never rush ahead.

## Step 5: Wrap up

After all selected concepts are discussed, offer:
- "Want to go deeper on any part?"
- "Want to see concepts from the next section?" (if more sections exist)

## Rules

- Respond in the user's language (follow CLAUDE.md preferences).
- Tone: knowledgeable colleague, not a lecturer.
- Do NOT dump full content at once.
- If the user asks a question mid-way, answer it, then resume where you left off.
