#!/usr/bin/env bash
set -e

REPO_DIR="$(cd "$(dirname "$0")" && pwd)"
BINARY_NAME="mdkb"
INSTALL_BIN="/usr/local/bin/$BINARY_NAME"
INSTALL_SCRIPTS="/usr/local/share/mdkb/scripts"
INSTALL_MAN="/usr/local/share/man/man1"
SKILLS_SRC="$REPO_DIR/.claude/skills"
SKILLS_DST="$HOME/.claude/skills"
MDKB_ROOT="$HOME/.mdkb"
TUTORIAL_SRC="$REPO_DIR/knowledge-templates/mdkb-getting-started.md"
TUTORIAL_DST="$MDKB_ROOT/knowledge/mdkb/getting-started/mdkb-getting-started.md"

echo "=== mdkb install ==="
echo ""

# 0. Install dependencies (jq for statusline)
echo "[0/8] Checking dependencies..."
case "$(uname)" in
    Darwin)
        BREW="$(command -v brew 2>/dev/null \
            || { [ -x /opt/homebrew/bin/brew ] && echo /opt/homebrew/bin/brew; } \
            || { [ -x /usr/local/bin/brew ]    && echo /usr/local/bin/brew; } \
            || true)"
        if [ -z "$BREW" ]; then
            echo "      Homebrew not found. Install it from https://brew.sh, then re-run install.sh"
            echo "      (needed for: jq, ncurses)"
            exit 1
        fi
        if ! command -v jq >/dev/null 2>&1; then
            echo "      Installing jq..."
            "$BREW" install jq
        else
            echo "      jq $(jq --version) — OK"
        fi
        # ncurses (Homebrew) provides ncursesw with proper UTF-8 wide-char support.
        # The system ncurses on macOS does not handle adjacent multi-byte chars correctly.
        if ! "$BREW" list ncurses >/dev/null 2>&1; then
            echo "      Installing ncurses (ncursesw) via Homebrew..."
            "$BREW" install ncurses
        else
            echo "      ncurses (Homebrew) — OK"
        fi
        ;;
    Linux)
        if ! command -v jq >/dev/null 2>&1; then
            echo "      jq not found — installing..."
            if command -v apt-get >/dev/null 2>&1; then
                sudo apt-get install -y -q jq
            elif command -v dnf >/dev/null 2>&1; then
                sudo dnf install -y -q jq
            elif command -v yum >/dev/null 2>&1; then
                sudo yum install -y -q jq
            elif command -v pacman >/dev/null 2>&1; then
                sudo pacman -S --noconfirm jq
            else
                echo "      Cannot detect package manager. Install jq manually."
            fi
        else
            echo "      jq $(jq --version) — OK"
        fi
        ;;
    *)
        if ! command -v jq >/dev/null 2>&1; then
            echo "      jq not found. Install manually: https://stedolan.github.io/jq/"
        else
            echo "      jq $(jq --version) — OK"
        fi
        ;;
esac

# 1. Build binary
echo "[1/8] Building mdkb..."
make -C "$REPO_DIR" clean --quiet 2>/dev/null || true
make -C "$REPO_DIR" --quiet
echo "      OK"

# 2. Install binary to /usr/local/bin
echo "[2/8] Installing binary to $INSTALL_BIN..."
if [ -w "$(dirname "$INSTALL_BIN")" ]; then
    install -m 755 "$REPO_DIR/$BINARY_NAME" "$INSTALL_BIN"
    mkdir -p "$INSTALL_SCRIPTS"
    cp "$REPO_DIR/scripts/"*.py "$REPO_DIR/scripts/"*.sh "$INSTALL_SCRIPTS/" 2>/dev/null || true
    mkdir -p "$INSTALL_MAN"
    install -m 644 "$REPO_DIR/man/man1/mdkb.1" "$INSTALL_MAN/mdkb.1"
else
    sudo install -m 755 "$REPO_DIR/$BINARY_NAME" "$INSTALL_BIN"
    sudo mkdir -p "$INSTALL_SCRIPTS"
    sudo cp "$REPO_DIR/scripts/"*.py "$REPO_DIR/scripts/"*.sh "$INSTALL_SCRIPTS/" 2>/dev/null || true
    sudo mkdir -p "$INSTALL_MAN"
    sudo install -m 644 "$REPO_DIR/man/man1/mdkb.1" "$INSTALL_MAN/mdkb.1"
fi
echo "      OK — $(mdkb -v)"

# 3. Install Claude Code skills (skip if already identical, use --force to overwrite)
echo "[3/8] Installing Claude Code skills to $SKILLS_DST..."
mkdir -p "$SKILLS_DST"
for skill_dir in "$SKILLS_SRC"/*/; do
    skill_name="$(basename "$skill_dir")"
    src="$skill_dir/SKILL.md"
    dst="$SKILLS_DST/$skill_name/SKILL.md"
    mkdir -p "$SKILLS_DST/$skill_name"
    if [ -f "$dst" ] && diff -q "$src" "$dst" >/dev/null 2>&1; then
        echo "      = $skill_name (unchanged)"
    elif [ -f "$dst" ] && [ "${FORCE:-0}" != "1" ]; then
        echo "      ! $skill_name (modified by user — skipped, use FORCE=1 to overwrite)"
    else
        cp "$src" "$dst"
        echo "      + $skill_name"
    fi
done

# 4. Initialize ~/.mdkb/ directory structure
echo "[4/8] Initializing $MDKB_ROOT..."
mkdir -p "$MDKB_ROOT/knowledge/_inbox"
mkdir -p "$MDKB_ROOT/knowledge/_archive"
mkdir -p "$MDKB_ROOT/knowledge/mdkb/getting-started"
mkdir -p "$MDKB_ROOT/archive"
echo "      OK"

# 5. Write tutorial note (only if it doesn't already exist)
echo "[5/8] Installing getting-started note..."
if [ -f "$TUTORIAL_DST" ]; then
    echo "      Skipped (already exists)"
else
    # stamp the current date into the note's sources.date field
    TODAY="$(date +%Y-%m-%d)"
    TIMESTAMP="$(date +%s)"
    sed -e "s/date: \"2026-01-01\"/date: \"$TODAY\"/" \
        -e "s/updated: 0/updated: $TIMESTAMP/" \
        "$TUTORIAL_SRC" > "$TUTORIAL_DST"
    echo "      Created: $TUTORIAL_DST"
fi

# 6. Install statusline script and register in Claude Code settings
echo "[6/8] Installing statusline..."
STATUSLINE_SRC="$REPO_DIR/scripts/mdkb-statusline.sh"
STATUSLINE_DST="$HOME/.claude/mdkb-statusline.sh"
CLAUDE_SETTINGS="$HOME/.claude/settings.json"

install -m 755 "$STATUSLINE_SRC" "$STATUSLINE_DST"

# Inject statusLine into settings.json if not already present
if [ -f "$CLAUDE_SETTINGS" ]; then
    if ! python3 -c "import json,sys; d=json.load(open('$CLAUDE_SETTINGS')); sys.exit(0 if 'statusLine' in d else 1)" 2>/dev/null; then
        python3 - "$CLAUDE_SETTINGS" "$STATUSLINE_DST" << 'PYEOF'
import json, sys
path, script = sys.argv[1], sys.argv[2]
with open(path) as f:
    d = json.load(f)
d['statusLine'] = {"type": "command", "command": f"sh {script}"}
with open(path, 'w') as f:
    json.dump(d, f, indent=2, ensure_ascii=False)
    f.write('\n')
PYEOF
        echo "      Registered statusLine in $CLAUDE_SETTINGS"
    else
        echo "      statusLine already configured — skipped"
    fi
else
    python3 - "$CLAUDE_SETTINGS" "$STATUSLINE_DST" << 'PYEOF'
import json, sys
path, script = sys.argv[1], sys.argv[2]
with open(path, 'w') as f:
    json.dump({"statusLine": {"type": "command", "command": f"sh {script}"}}, f, indent=2)
    f.write('\n')
PYEOF
    echo "      Created $CLAUDE_SETTINGS with statusLine"
fi
echo "      OK — statusline at $STATUSLINE_DST"

# 7. Rebuild search index
echo "[7/8] Building search index..."
mdkb --reindex 2>/dev/null || true
echo "      OK"

echo ""
echo "=== Done! ==="
echo ""
echo "Quick start:"
echo "  1. Enable auto-sync (once):  mdkb --sync-on"
echo "  2. Open knowledge browser:   mdkb"
echo "  3. In TUI — press L to load a note, then inside Claude: /mdkb-read"
echo "  4. Or press ? in TUI for full key binding reference"
echo ""
echo "Getting-started note is at:"
echo "  $TUTORIAL_DST"
echo "Load it in mdkb TUI and run /mdkb-read to start the guided tour."
