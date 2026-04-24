---
name: mdkb-workflow
description: "Execute automated workflows from loaded knowledge notes. Use when: '/mdkb-workflow', 'run workflow', 'execute SOP', 'automate from notes'."
version: "1.0"
author: "Anderson Chang"
metadata:
  category: "utility"
---

# mdkb-workflow — Workflow SOP Execution

This skill finds workflow-type knowledge notes loaded in the system prompt and executes them as automated SOPs.

**You MUST use the Read tool to read the actual file content from the paths listed in the system prompt before doing anything.**

Look for entries in the system prompt that match this pattern:
```
The following knowledge files are loaded. Use the Read tool to access full content when needed:
1. /path/to/file.md — Title
2. /path/to/file2.md — Title 2
```

If no knowledge file paths are found in the system prompt, tell the user: "No knowledge files loaded. Use the L key in mdkb TUI to load notes first."

## Step 0: Read and filter

Read ALL knowledge files from the system prompt. Then filter for **workflow-type notes only**:
- Check frontmatter for `type: workflow` OR tags containing `type:workflow`
- If no workflow-type notes are found, tell the user: "No workflow-type notes loaded. The loaded notes are knowledge/code type."

## Step 1: List available workflows

Show a numbered list of executable workflows:

```
Available workflows:
1. [title] — brief description of what this workflow automates
2. [title] — brief description
```

## Step 2: User selects workflow

The user picks one (or more) workflows to execute. Confirm before proceeding:
- Show the full SOP steps from the note
- Ask: "Execute this workflow?" (the user must confirm)

## Step 3: Execute

Parse the workflow note's SOP steps and execute them sequentially:

1. **Read each step** from the note (look for numbered lists, `## Step N` headings, or ordered instructions)
2. **Execute each step** using the appropriate tool:
   - Shell commands → Bash tool
   - File modifications → Edit/Write tools
   - Code compilation → Bash tool
   - Test execution → Bash tool
3. **Report status** after each step:
   - What was executed
   - Output/result (concise)
   - Pass/fail status
4. **On error**: Stop, show the error, and ask user how to proceed:
   - Retry
   - Skip this step
   - Abort workflow

## Step 4: Summary

After all steps complete, show a summary:

```
Workflow: [title]
Steps: 5/5 completed
Status: SUCCESS (or PARTIAL — 4/5 passed)

Results:
1. [step] — passed
2. [step] — passed
3. [step] — FAILED: [brief error]
4. [step] — skipped
5. [step] — passed
```

## Rules

- Respond in the user's language (follow CLAUDE.md preferences)
- NEVER execute without user confirmation first
- Show each step BEFORE executing it
- Stop on errors — do not silently continue
- If a step is ambiguous or unsafe, ask the user before proceeding
- Treat the workflow note as the source of truth for what to do, but verify file paths and commands exist before running
- If the workflow references files that don't exist, warn the user and ask how to proceed
