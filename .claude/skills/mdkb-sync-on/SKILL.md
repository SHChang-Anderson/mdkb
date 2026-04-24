---
name: mdkb-sync-on
description: "Enable mdkb auto-sync hook. Use when: '/mdkb-sync-on', 'enable mdkb sync', 'turn on sync'."
version: "1.0"
author: "Anderson Chang"
metadata:
  category: "utility"
---

# mdkb-sync-on — Enable Auto-Sync

Run:

```bash
mdkb --sync-on 2>/dev/null
```

Registers a Stop hook so conversations are automatically ingested and knowledge-extracted on session end.
