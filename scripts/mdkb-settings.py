#!/usr/bin/env python3
"""Add or remove mdkb Stop hook from Claude Code settings.json"""
import json
import os
import sys

SETTINGS_PATH = os.path.expanduser("~/.claude/settings.json")
HOOK_MARKER = "mdkb-sync"

# Resolve hook script path: prefer installed location, fall back to sibling dir
_installed = "/usr/local/share/mdkb/scripts/mdkb-sync-hook.sh"
_local = os.path.join(os.path.dirname(__file__), "mdkb-sync-hook.sh")
HOOK_COMMAND = _installed if os.path.exists(_installed) else _local

def load_settings():
    try:
        with open(SETTINGS_PATH) as f:
            return json.load(f)
    except (FileNotFoundError, json.JSONDecodeError):
        return {}

def save_settings(data):
    with open(SETTINGS_PATH, "w") as f:
        json.dump(data, f, indent=2, ensure_ascii=False)
        f.write("\n")

def has_mdkb_hook(hook_entry):
    """Check if a hook entry contains mdkb-sync command"""
    for h in hook_entry.get("hooks", []):
        if HOOK_MARKER in h.get("command", ""):
            return True
    return False

def add_hook():
    data = load_settings()
    hooks = data.setdefault("hooks", {})
    stop_list = hooks.get("Stop", [])

    # Already registered?
    if any(has_mdkb_hook(entry) for entry in stop_list):
        return

    stop_list.append({
        "hooks": [{
            "type": "command",
            "command": HOOK_COMMAND,
            "timeout": 10
        }]
    })
    hooks["Stop"] = stop_list
    save_settings(data)

def remove_hook():
    data = load_settings()
    hooks = data.get("hooks", {})
    stop_list = hooks.get("Stop", [])

    new_list = [entry for entry in stop_list if not has_mdkb_hook(entry)]

    if new_list:
        hooks["Stop"] = new_list
    elif "Stop" in hooks:
        del hooks["Stop"]

    save_settings(data)

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: mdkb-settings.py [add|remove]", file=sys.stderr)
        sys.exit(1)

    action = sys.argv[1]
    if action == "add":
        add_hook()
    elif action == "remove":
        remove_hook()
    else:
        print(f"Unknown action: {action}", file=sys.stderr)
        sys.exit(1)
