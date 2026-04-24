#!/bin/sh
# Claude Code status line: mdkb sync status + model name + context usage

input=$(cat)

model=$(echo "$input" | jq -r '.model.display_name // "Claude"')
used=$(echo "$input" | jq -r '.context_window.used_percentage // empty')

# Check mdkb sync status
if [ -f "$HOME/.mdkb/.sync-enabled" ]; then
  sync_label="\033[1;34mmdkb-sync\033[0m"
else
  sync_label="\033[2mmdkb-off\033[0m"
fi

printf "%b \033[2m|\033[0m \033[1m%s\033[0m" "$sync_label" "$model"
