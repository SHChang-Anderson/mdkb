#!/bin/sh
# mdkb Stop hook: auto-sync Claude Code conversation to mdkb notes

LOG="$HOME/.mdkb/sync.log"

log() { echo "$(date '+%H:%M:%S') $*" >> "$LOG"; }

# Check toggle
if [ ! -f "$HOME/.mdkb/.sync-enabled" ]; then
    exit 0
fi

# Resolve mdkb binary and scripts dir
MDKB_BIN="$(command -v mdkb 2>/dev/null)"
if [ -z "$MDKB_BIN" ]; then
    log "ERROR: mdkb binary not found in PATH"
    exit 1
fi

MDKB_SCRIPTS="/usr/local/share/mdkb/scripts"
if [ ! -d "$MDKB_SCRIPTS" ]; then
    # fallback: scripts next to this hook
    MDKB_SCRIPTS="$(dirname "$0")"
fi

# Read hook input
INPUT=$(cat)
log "Hook fired. Input length: $(echo "$INPUT" | wc -c)"

# Extract transcript_path
TRANSCRIPT=$(echo "$INPUT" | sed -n 's/.*"transcript_path":"\([^"]*\)".*/\1/p')
log "Transcript: $TRANSCRIPT"

if [ -z "$TRANSCRIPT" ] || [ ! -f "$TRANSCRIPT" ]; then
    log "No transcript found, exiting"
    exit 0
fi

# Ingest
MARKER="/tmp/.mdkb-sync-marker-$$"
touch "$MARKER"
"$MDKB_BIN" -i "$TRANSCRIPT" >>"$LOG" 2>&1
log "Ingest done"

# Find new note
LATEST=$(find "$HOME/.mdkb/archive" -name "*.md" -newer "$MARKER" -type f 2>/dev/null | head -1)
rm -f "$MARKER"
log "Latest note: $LATEST"

if [ -n "$LATEST" ]; then
    log "Starting extraction..."
    python3 "$MDKB_SCRIPTS/mdkb-categorize.py" "$LATEST" >>"$LOG" 2>&1 &
    log "Extraction launched (PID: $!)"
else
    log "No new note found, skipping extraction"
fi

exit 0
