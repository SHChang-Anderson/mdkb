#!/usr/bin/env python3
"""
mdkb-categorize: AI-powered knowledge extraction from Claude Code conversations.

Reads a conversation markdown note, calls Claude API to extract knowledge,
and creates/updates structured knowledge notes in ~/.mdkb/knowledge/<repo>/<topic>/.

Usage: mdkb-categorize.py <conversation.md> [--dry-run]
"""
import json
import os
import re
import subprocess
import sys
import time

KNOWLEDGE_DIR = os.path.expanduser("~/.mdkb/knowledge")
TAG_INDEX_PATH = os.path.expanduser("~/.mdkb/.tag-index")
MIN_DELTA_LEN = 200  # skip extraction if delta is too small
SPLIT_WORD_LIMIT = 1200  # auto-split notes exceeding this word count


def call_claude(prompt):
    """Call Claude CLI in non-interactive print mode."""
    result = subprocess.run(
        ["claude", "-p", "--no-session-persistence", "--model", "sonnet"],
        input=prompt,
        capture_output=True,
        text=True,
        timeout=300,
    )
    if result.returncode != 0:
        print(f"[extract] claude -p error: {result.stderr[:200]}", file=sys.stderr)
        return None
    return result.stdout.strip() or None


def load_tag_index():
    """Load existing tags from tag index file."""
    try:
        with open(TAG_INDEX_PATH) as f:
            return sorted(set(line.strip() for line in f if line.strip()))
    except FileNotFoundError:
        return []


def save_tag_index(tags):
    """Save tags to index file (deduplicated, sorted)."""
    os.makedirs(os.path.dirname(TAG_INDEX_PATH), exist_ok=True)
    existing = set(load_tag_index())
    existing.update(tags)
    with open(TAG_INDEX_PATH, "w") as f:
        for tag in sorted(existing):
            f.write(tag + "\n")


def scan_existing_knowledge():
    """Scan knowledge/ directory and return list of existing notes with metadata."""
    notes = []
    if not os.path.isdir(KNOWLEDGE_DIR):
        return notes

    for root, _dirs, files in os.walk(KNOWLEDGE_DIR):
        for fname in files:
            if not fname.endswith(".md"):
                continue
            rel = os.path.relpath(os.path.join(root, fname), KNOWLEDGE_DIR)
            parts = rel.split(os.sep)
            repo = parts[0] if len(parts) > 1 else "misc"
            topic = parts[1] if len(parts) > 2 else "general"

            # Read title from first heading
            fpath = os.path.join(root, fname)
            title = fname.replace(".md", "").replace("-", " ")
            try:
                with open(fpath, encoding="utf-8") as f:
                    for line in f:
                        if line.startswith("# "):
                            title = line[2:].strip()
                            break
            except Exception:
                pass

            notes.append({
                "path": rel,
                "repo": repo,
                "topic": topic,
                "title": title,
            })

    return notes


def parse_note(path):
    """Parse markdown note: extract YAML front-matter and content."""
    with open(path, encoding="utf-8") as f:
        text = f.read()

    if not text.startswith("---"):
        return None, text

    end = text.find("\n---", 3)
    if end < 0:
        return None, text

    fm_text = text[4:end]
    content = text[end + 4:]

    fm = {}
    for line in fm_text.split("\n"):
        m = re.match(r'^(\w+):\s*(.+)$', line)
        if m:
            key, val = m.group(1), m.group(2).strip()
            if val.startswith('"') and val.endswith('"'):
                val = val[1:-1].replace('\\"', '"')
            if val.startswith("[") and val.endswith("]"):
                val = [v.strip().strip('"') for v in val[1:-1].split(",")]
            fm[key] = val

    return fm, content




def write_knowledge_note(action):
    """Create or update a knowledge note based on AI action."""
    if action["action"] == "create":
        repo = action["repo"]
        topic = action["topic"]
        filename = action["filename"]
        title = action["title"]
        tags = action.get("tags", [])
        content = action["content"]
        session_id = action.get("session_id", "")
        date = action.get("date", "")
        note_type = action.get("note_type", "knowledge")

        # Ensure type:xxx tag is present
        type_tag = f"type:{note_type}"
        if type_tag not in tags:
            tags.append(type_tag)

        # Build path — new notes go to _inbox/ for human triage
        # Sanitize: strip any accidental path separators from filename
        filename = os.path.basename(filename)
        dirpath = os.path.join(KNOWLEDGE_DIR, "_inbox")
        os.makedirs(dirpath, exist_ok=True)
        filepath = os.path.join(dirpath, filename)

        # Write note with front-matter
        with open(filepath, "w", encoding="utf-8") as f:
            f.write("---\n")
            f.write(f'title: "{title}"\n')
            f.write(f"type: {note_type}\n")
            f.write(f'repo: "{repo}"\n')
            f.write(f'topic: "{topic}"\n')
            f.write(f'tags: [{", ".join(tags)}]\n')
            f.write(f"status: draft\n")
            f.write(f"sources:\n")
            f.write(f'  - session: "{session_id}"\n')
            f.write(f'    date: "{date}"\n')
            f.write(f"updated: {int(time.time())}\n")
            f.write("---\n\n")
            f.write(f"# {title}\n\n")
            f.write(content)
            f.write("\n")

        print(f"[extract] Created: {os.path.relpath(filepath, KNOWLEDGE_DIR)}", file=sys.stderr)
        return filepath

    elif action["action"] == "update":
        rel_path = action["path"]
        append_content = action["append"]
        session_id = action.get("session_id", "")
        date = action.get("date", "")

        filepath = os.path.join(KNOWLEDGE_DIR, rel_path)
        if not os.path.isfile(filepath):
            print(f"[extract] Warning: target not found for update: {rel_path}", file=sys.stderr)
            return None

        with open(filepath, encoding="utf-8") as f:
            text = f.read()

        # Add source to front-matter
        end = text.find("\n---", 3)
        if end > 0:
            fm_part = text[:end]
            rest = text[end:]
            # Add new source entry before 'updated:'
            source_line = f'  - session: "{session_id}"\n    date: "{date}"\n'
            if f'session: "{session_id}"' not in fm_part:
                fm_part = fm_part.replace(
                    f"updated:",
                    f"{source_line}updated:"
                )
            # Update timestamp
            fm_part = re.sub(r'updated: \d+', f'updated: {int(time.time())}', fm_part)
            text = fm_part + rest

        # Append new content
        text = text.rstrip() + "\n\n" + append_content + "\n"

        with open(filepath, "w", encoding="utf-8") as f:
            f.write(text)

        print(f"[extract] Updated: {rel_path}", file=sys.stderr)
        return filepath


def count_words(text):
    """Count whitespace-separated tokens in text."""
    return len(text.split())


def parse_body_sections(body):
    """Split body at '## ' boundaries.

    Returns list of (heading_line, section_body) tuples.
    heading='' for preamble before first ## heading.
    """
    sections = []
    current_heading = ""
    current_lines = []

    for line in body.split("\n"):
        if line.startswith("## "):
            # Flush previous section
            if current_lines or current_heading:
                sections.append((current_heading, "\n".join(current_lines)))
            current_heading = line
            current_lines = []
        else:
            current_lines.append(line)

    # Flush last section
    if current_lines or current_heading:
        sections.append((current_heading, "\n".join(current_lines)))

    return sections


def group_sections_by_llm(sections):
    """Ask LLM to group semantically related sections together.

    Returns list of groups, each group is a list of section indices.
    Returns None on failure (caller should fallback to greedy packing).
    """
    if len(sections) <= 2:
        return None  # not worth an API call for 2 sections

    # Build section summaries: heading + first 3 lines of body (save tokens)
    summaries = []
    for i, (heading, body) in enumerate(sections):
        preview_lines = [l for l in body.split("\n") if l.strip()][:3]
        preview = "\n   ".join(preview_lines)
        label = heading if heading else "(preamble)"
        summaries.append(f"{i}: {label}\n   {preview}")

    section_list = "\n".join(summaries)

    prompt = f"""Given these sections from a knowledge note, group semantically related sections together.
Each group will become a separate file, so sections that need each other for context should stay together.

Sections:
{section_list}

Reply with JSON only: {{"groups": [[0,1], [2], [3]]}}
Rules:
- Keep semantically related sections together (e.g. problem + solution, design + implementation)
- Each group should ideally be under {SPLIT_WORD_LIMIT} words, but semantic coherence takes priority over word count
- Preserve section order within each group
- Every section index must appear exactly once
- Output ONLY valid JSON, no markdown fences, no explanation"""

    response = call_claude(prompt)
    if not response:
        print("[split] LLM grouping failed: no response, falling back to greedy", file=sys.stderr)
        return None

    try:
        clean = response.strip()
        if clean.startswith("```"):
            clean = re.sub(r'^```\w*\n?', '', clean)
            clean = re.sub(r'\n?```$', '', clean)
        result = json.loads(clean)
        groups = result.get("groups") or result.get("result")
    except (json.JSONDecodeError, AttributeError):
        print(f"[split] LLM grouping failed: invalid JSON, falling back to greedy", file=sys.stderr)
        return None

    if not isinstance(groups, list):
        print("[split] LLM grouping failed: groups is not a list, falling back to greedy", file=sys.stderr)
        return None

    # Validate: every index appears exactly once
    all_indices = sorted(idx for group in groups for idx in group)
    expected = list(range(len(sections)))
    if all_indices != expected:
        print(f"[split] LLM grouping failed: indices {all_indices} != expected {expected}, "
              f"falling back to greedy", file=sys.stderr)
        return None

    # Validate: indices within each group are in ascending order
    for group in groups:
        if group != sorted(group):
            print(f"[split] LLM grouping failed: group {group} not in order, "
                  f"falling back to greedy", file=sys.stderr)
            return None

    if len(groups) <= 1:
        return None  # LLM says don't split

    print(f"[split] LLM grouped {len(sections)} sections into {len(groups)} parts: {groups}",
          file=sys.stderr)
    return groups


def split_note_if_oversized(filepath):
    """Post-process: if note body exceeds SPLIT_WORD_LIMIT, split at ## boundaries.

    When API config is provided, uses LLM to group semantically related sections.
    Falls back to greedy packing when LLM is unavailable or fails.

    Splits into {stem}-part{N}.md files, each keeping original frontmatter
    with title suffixed. Deletes the original oversized file.
    Returns list of final file paths.
    """
    with open(filepath, encoding="utf-8") as f:
        text = f.read()

    # Separate frontmatter from body
    if not text.startswith("---"):
        return [filepath]

    end = text.find("\n---", 3)
    if end < 0:
        return [filepath]

    fm_text = text[:end + 4]  # includes opening and closing ---
    body = text[end + 4:].lstrip("\n")

    if count_words(body) <= SPLIT_WORD_LIMIT:
        return [filepath]

    sections = parse_body_sections(body)
    if len(sections) <= 1:
        print(f"[split] Warning: {os.path.basename(filepath)} exceeds word limit "
              f"but has no ## headings to split on", file=sys.stderr)
        return [filepath]

    # Try LLM-based semantic grouping first, fallback to greedy packing
    groups = group_sections_by_llm(sections)

    if groups:
        # LLM grouping: assemble parts from group indices
        parts = [
            [sections[i] for i in group]
            for group in groups
        ]
    else:
        # Greedy packing fallback
        parts = []
        current_part = []
        current_words = 0

        for heading, content in sections:
            section_text = (heading + "\n" + content).strip() if heading else content.strip()
            section_words = count_words(section_text)

            if current_part and current_words + section_words > SPLIT_WORD_LIMIT:
                parts.append(current_part)
                current_part = [(heading, content)]
                current_words = section_words
            else:
                current_part.append((heading, content))
                current_words += section_words

        if current_part:
            parts.append(current_part)

    if len(parts) <= 1:
        return [filepath]

    # Extract original title from frontmatter
    title_match = re.search(r'title:\s*"([^"]*)"', fm_text)
    orig_title = title_match.group(1) if title_match else "Untitled"

    # Write part files
    stem = os.path.splitext(filepath)[0]
    total = len(parts)
    created = []

    for i, part_sections in enumerate(parts, 1):
        part_title = f"{orig_title} (part {i}/{total})"
        part_fm = re.sub(
            r'title:\s*"[^"]*"',
            f'title: "{part_title}"',
            fm_text
        )

        part_body = ""
        for heading, content in part_sections:
            if heading:
                part_body += heading + "\n" + content + "\n\n"
            else:
                part_body += content + "\n\n"

        part_path = f"{stem}-part{i}.md"
        with open(part_path, "w", encoding="utf-8") as f:
            f.write(part_fm)
            f.write("\n")
            f.write(part_body.rstrip() + "\n")

        created.append(part_path)
        print(f"[split] Created: {os.path.relpath(part_path, KNOWLEDGE_DIR)} "
              f"({count_words(part_body)} words)", file=sys.stderr)

    # Delete original oversized file
    os.unlink(filepath)
    print(f"[split] Removed original: {os.path.relpath(filepath, KNOWLEDGE_DIR)} "
          f"→ split into {total} parts", file=sys.stderr)

    return created


def mark_extracted(note_path, offset):
    """Store extracted byte offset in conversation note."""
    with open(note_path, encoding="utf-8") as f:
        text = f.read()

    end = text.find("\n---", 3)
    if end < 0:
        return

    fm_part = text[4:end]
    rest = text[end + 4:]

    # Replace or add extracted_offset
    if "extracted_offset:" in fm_part:
        fm_part = re.sub(r'extracted_offset: \d+', f'extracted_offset: {offset}', fm_part)
    else:
        fm_part += f"\nextracted_offset: {offset}"

    with open(note_path, "w", encoding="utf-8") as f:
        f.write(f"---\n{fm_part}\n---{rest}")


def main():
    if len(sys.argv) < 2:
        print("Usage: mdkb-categorize.py <conversation.md> [--dry-run]", file=sys.stderr)
        sys.exit(1)

    note_path = sys.argv[1]
    dry_run = "--dry-run" in sys.argv

    if not os.path.isfile(note_path):
        print(f"[extract] File not found: {note_path}", file=sys.stderr)
        sys.exit(1)

    # Parse conversation note
    fm, content = parse_note(note_path)
    if not fm:
        print("[extract] No front-matter found", file=sys.stderr)
        sys.exit(1)

    # Get delta: only new content since last extraction
    # Store offset in a sidecar file (.offset) to survive ingest overwrites
    offset_file = note_path + ".offset"
    prev_offset = 0
    try:
        with open(offset_file) as f:
            prev_offset = int(f.read().strip())
    except (FileNotFoundError, ValueError):
        pass

    total_len = len(content)
    delta = content[prev_offset:]

    if len(delta.strip()) < MIN_DELTA_LEN:
        print(f"[extract] Delta too small ({len(delta.strip())} chars), skipping", file=sys.stderr)
        sys.exit(0)

    # Scan existing knowledge notes
    existing_notes = scan_existing_knowledge()
    existing_tags = load_tag_index()

    # Build context
    title = fm.get("title", "Unknown")
    cwd = fm.get("cwd", "Unknown")
    session_id = fm.get("session_id", "unknown")
    date = fm.get("timestamp", "")
    if isinstance(date, str) and date.isdigit():
        date = time.strftime("%Y-%m-%d", time.localtime(int(date)))

    content_text = delta
    print(f"[extract] Delta: {prev_offset} → {total_len} ({len(delta)} chars new)", file=sys.stderr)

    existing_notes_str = ""
    if existing_notes:
        existing_notes_str = "\n".join(
            f"- {n['path']} — {n['title']}"
            for n in existing_notes
        )
    else:
        existing_notes_str = "(none yet)"

    tag_list = ", ".join(existing_tags) if existing_tags else "(none yet)"

    prompt = f"""You are a knowledge base extraction engine. Given NEW content from a Claude Code conversation, extract the key knowledge and output structured JSON.

Your task:
1. Identify the key knowledge, decisions, findings, or solutions in this NEW content
2. For each distinct topic, decide whether to CREATE a new knowledge note or UPDATE an existing one
3. Output JSON with an "actions" array
4. This is INCREMENTAL content — only the latest exchanges, not the full conversation

Rules:
- Extract knowledge, NOT conversation. Write as documentation, not dialogue.
- Use the conversation's language for content (Traditional Chinese if the conversation is in Chinese)
- Filenames must be lowercase kebab-case English
- IMPORTANT: Split into MULTIPLE notes by distinct topic. A long conversation typically produces 3-8 notes.
- Do NOT dump everything into one note. Each note = ONE focused topic.
- Only UPDATE an existing note if the new info is about the EXACT same specific topic. Otherwise CREATE a new note.
- repo should match the actual project (e.g. "mdkb" for mdkb development, "my-project" for your project work)
- Include relevant code snippets and technical details
- Add a "## Resume" section with the claude --resume command
- Each note MUST include a "note_type" field with one of: "knowledge" or "code"
  - "knowledge": conceptual knowledge, feature design, architecture, decisions, explanations
  - "code": code implementation details, modifications, bug fixes, diffs, code snippets with file paths
- Do NOT use "workflow" as note_type — workflow notes are created separately via /mdkb-workflow-create
- Add "type:{{note_type}}" to the tags array (e.g. tags: ["pcap", "type:code"])

CRITICAL for "code" notes:
- Every code reference MUST include the ABSOLUTE file path and line number, formatted as `filepath:line`
- Use CWD to construct absolute paths. The CWD for this conversation is: {cwd}
- Include a "## 相關程式碼位置" (or "## Code Locations") section with a table of all referenced files, line numbers, and descriptions
- When showing code snippets, always prefix with a comment showing the absolute path and line number
- This is essential — the notes will be loaded into Claude Code as context, and it needs exact locations to navigate directly to the source code and verify the current state

Existing knowledge notes:
{existing_notes_str}

Existing tags: {tag_list}

Conversation metadata:
- Title: {title}
- CWD: {cwd}
- Session: {session_id}
- Date: {date}

New conversation content (since last extraction):
{content_text}

Output JSON format:
{{
  "actions": [
    {{
      "action": "create",
      "repo": "my-project",
      "topic": "debugging",
      "filename": "connection-handling.md",
      "title": "Connection timeout handling",
      "note_type": "code",
      "tags": ["networking", "timeout", "type:code"],
      "content": "## 重點\\n\\n- point 1\\n- point 2\\n\\n## 相關程式碼位置\\n\\n| 檔案 | 行號 | 說明 |\\n|------|------|------|\\n| `/home/user/project/src/foo.c` | 123-145 | function_name() 主邏輯 |\\n| `/home/user/project/include/foo.h` | 42 | struct 定義 |\\n\\n## 程式碼片段\\n\\n```c\\n// /home/user/project/src/foo.c:123\\nstatic void function_name(void) {{\\n    // ...\\n}}\\n```\\n\\n## Resume\\n\\n- `claude --resume xxx`"
    }},
    {{
      "action": "update",
      "path": "my-project/networking/upload-flow.md",
      "append": "## {{date}} 補充\\n\\n- new finding"
    }}
  ]
}}

Output ONLY valid JSON, no markdown fences, no explanation."""

    print(f"[extract] Calling API for: {os.path.basename(note_path)}", file=sys.stderr)

    response = call_claude(prompt)
    if not response:
        print("[extract] No response from API", file=sys.stderr)
        sys.exit(1)

    # Parse JSON
    try:
        clean = response.strip()
        if clean.startswith("```"):
            clean = re.sub(r'^```\w*\n?', '', clean)
            clean = re.sub(r'\n?```$', '', clean)
        result = json.loads(clean)
    except json.JSONDecodeError:
        print(f"[extract] Invalid JSON: {response[:300]}", file=sys.stderr)
        sys.exit(1)

    actions = result.get("actions", [])
    if not actions:
        print("[extract] No actions returned", file=sys.stderr)
        sys.exit(0)

    print(f"[extract] {len(actions)} action(s) to execute", file=sys.stderr)

    if dry_run:
        print(json.dumps(result, ensure_ascii=False, indent=2))
        sys.exit(0)

    # Execute actions
    all_tags = []
    for action in actions:
        action["session_id"] = session_id
        action["date"] = date
        filepath = write_knowledge_note(action)
        if filepath:
            split_note_if_oversized(filepath)
        all_tags.extend(action.get("tags", []))
        if action.get("repo"):
            all_tags.append(action["repo"])
        if action.get("topic"):
            all_tags.append(action["topic"])

    # Save offset to sidecar file
    with open(offset_file, "w") as f:
        f.write(str(total_len))

    # Update tag index
    save_tag_index(all_tags)
    print("[extract] Done", file=sys.stderr)


if __name__ == "__main__":
    main()
