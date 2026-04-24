---
name: mdkb-workflow-create
description: "Create a new workflow note via interactive Q&A. Use when: '/mdkb-workflow-create', 'create workflow', 'new workflow', 'build SOP', 'make automation'."
version: "1.0"
author: "Anderson Chang"
metadata:
  category: "utility"
---

# mdkb-workflow-create — Interactive Workflow Builder

Build a new workflow (SOP) note through guided Q&A, test it, and save to mdkb knowledge base.

## Phase 1: Basic Info

Ask these questions ONE AT A TIME using AskUserQuestion. Do not ask all at once.

**Q1: Title**
Ask: "Workflow 標題是什麼？" — This becomes the note title and `# heading`.

**Q2: Repo & Topic**
Ask: "這個 workflow 屬於哪個 repo / topic？" — Provide existing repos/topics as options if possible.
Run `ls ~/.mdkb/knowledge/` to list repos, then `ls ~/.mdkb/knowledge/<repo>/` for topics.
If the user names a new repo/topic, that's fine — the directory will be created.

**Q3: Tags**
Ask: "要加哪些 tags？（空格分隔，type:workflow 會自動加）"

## Phase 2: Step-by-Step Definition

Now build the SOP steps interactively. For each step:

**Ask**: "步驟 {N}：要做什麼？（輸入 `done` 結束）"

For each step the user describes, clarify:
1. **What** — the action description (user already provided)
2. **How** — the actual command or action. Ask: "這步的指令是什麼？" Examples:
   - Shell command: `make -C ~/my-project clean && make -C ~/my-project`
   - File edit: "Edit /path/to/file.c, change X to Y"
   - Conditional: "If X, then do Y"
3. **Verify** — optional validation. Ask: "怎麼確認這步成功？（Enter 跳過）" Examples:
   - Check exit code (default for shell commands)
   - Check file exists
   - Grep for expected output

After each step, show a summary:
```
步驟 {N}: {description}
  指令: {command}
  驗證: {verification}
```

Repeat until the user says `done`, `完成`, or `結束`.

## Phase 3: Review

Show the complete workflow:

```
=== Workflow Preview ===
Title: {title}
Repo: {repo} / Topic: {topic}
Tags: {tags}

Steps:
1. {description}
   $ {command}
   ✓ {verification}

2. {description}
   $ {command}
   ✓ {verification}

...
```

Ask: "確認這個 workflow？要修改哪個步驟嗎？"
- If the user wants to edit a step, modify it and re-show
- If confirmed, proceed to Phase 4

## Phase 4: Test Run

Tell the user: "開始測試 workflow..."

Execute each step in order:
1. Show the step being executed
2. Run the command via Bash tool
3. Run the verification (if any)
4. Report pass/fail for each step

After all steps:
```
Test Results:
1. {description} — PASS
2. {description} — PASS
3. {description} — FAIL: {error}
```

- If ALL pass: "測試全部通過，準備儲存。"
- If any fail: Show the error and ask:
  - "修正後重測？"
  - "跳過失敗的步驟繼續儲存？"
  - "放棄？"

## Phase 5: Save

Write the workflow note to `~/.mdkb/knowledge/{repo}/{topic}/{filename}.md`

**Filename**: derive from title, lowercase kebab-case, e.g. "Build and Test mdkb" → `build-and-test-mdkb.md`

**File format**:

```markdown
---
title: "{title}"
type: workflow
repo: "{repo}"
topic: "{topic}"
tags: [{tags}, type:workflow]
sources:
  - session: "{session_id}"
    date: "{date}"
updated: {timestamp}
---

# {title}

## Overview

{one-line description generated from the steps}

## Steps

### Step 1: {description}

```bash
{command}
```

**Verify**: {verification}

### Step 2: {description}

```bash
{command}
```

**Verify**: {verification}

...

## Notes

- Created via `/mdkb-workflow-create`
- Tested: {date}
- All steps passed / {N}/{M} steps passed

## Resume

- `claude --resume {session_id}`
```

After saving, confirm:
"Workflow 已儲存到 `{filepath}`，可以在 mdkb TUI 用 `T` 篩選 workflow 類型找到，或用 `/mdkb-workflow` 執行。"

## Rules

- Respond in the user's language (follow CLAUDE.md preferences)
- ONE question at a time — never dump all questions
- Show progress: "步驟 1/N 已定義" after each step
- If the user gives a vague step like "build the project", ask for the specific command
- Always test before saving — never save an untested workflow
- Use `time.strftime("%Y-%m-%d")` for date, `int(time.time())` for timestamp
- Get the current session ID from environment or use "manual" as fallback
- The saved file MUST have `type: workflow` in frontmatter and `type:workflow` in tags
