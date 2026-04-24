#!/usr/bin/env bash
set -e

BINARY="/usr/local/bin/mdkb"
INSTALL_SCRIPTS="/usr/local/share/mdkb"
MAN_PAGE="/usr/local/share/man/man1/mdkb.1"
STATUSLINE="$HOME/.claude/mdkb-statusline.sh"
CLAUDE_SETTINGS="$HOME/.claude/settings.json"
SKILLS_DIR="$HOME/.claude/skills"
MDKB_ROOT="$HOME/.mdkb"

SKILLS=(
    mdkb-read
    mdkb-sync-on
    mdkb-sync-off
    mdkb-tidy
    mdkb-workflow
    mdkb-workflow-create
)

echo "=== mdkb uninstall ==="
echo ""

# 1. Disable sync hook before removing binary
if [ -f "$MDKB_ROOT/.sync-enabled" ] && command -v mdkb >/dev/null 2>&1; then
    echo "[1/5] Disabling sync hook..."
    mdkb --sync-off 2>/dev/null || true
    echo "      OK"
else
    echo "[1/5] Sync hook not active — skipped"
fi

# 2. Remove binary and scripts
echo "[2/5] Removing binary and scripts..."
if [ -f "$BINARY" ]; then
    if [ -w "$(dirname "$BINARY")" ]; then
        rm -f "$BINARY"
        rm -rf "$INSTALL_SCRIPTS"
        rm -f "$MAN_PAGE"
    else
        sudo rm -f "$BINARY"
        sudo rm -rf "$INSTALL_SCRIPTS"
        sudo rm -f "$MAN_PAGE"
    fi
    echo "      OK"
else
    echo "      Binary not found — skipped"
fi

# 3. Remove Claude Code skills
echo "[3/5] Removing Claude Code skills..."
for skill in "${SKILLS[@]}"; do
    if [ -d "$SKILLS_DIR/$skill" ]; then
        rm -rf "$SKILLS_DIR/$skill"
        echo "      - $skill"
    fi
done

# 4. Remove statusline script and settings entry
echo "[4/5] Removing statusline..."
rm -f "$STATUSLINE"
if [ -f "$CLAUDE_SETTINGS" ]; then
    python3 - "$CLAUDE_SETTINGS" << 'PYEOF' 2>/dev/null || true
import json, sys
path = sys.argv[1]
with open(path) as f:
    d = json.load(f)
d.pop('statusLine', None)
with open(path, 'w') as f:
    json.dump(d, f, indent=2, ensure_ascii=False)
    f.write('\n')
PYEOF
fi
echo "      OK"

# 5. Remove knowledge base (ask unless --purge passed)
echo "[5/5] Knowledge base (~/.mdkb/)..."
if [ "${PURGE:-0}" = "1" ]; then
    rm -rf "$MDKB_ROOT"
    echo "      Deleted (PURGE=1)"
else
    echo "      Kept (your notes are safe)"
    echo "      To delete: PURGE=1 ./uninstall.sh  or  rm -rf ~/.mdkb/"
fi

echo ""
echo "=== Done! ==="
