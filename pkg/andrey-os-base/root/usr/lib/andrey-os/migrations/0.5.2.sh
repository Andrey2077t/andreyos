#!/bin/bash
# 0.4.11: fixa Perfis de Sessão no Menu Iniciar de quem já personalizou os favoritos e ativa os atalhos novos.
F="${XDG_CONFIG_HOME:-$HOME/.config}/andrey-os/start/pinned.json"
if [ -f "$F" ]; then
python3 - "$F" <<'PY' || true
import json, os, sys, tempfile
path = sys.argv[1]
try:
    with open(path, encoding="utf-8") as fh:
        items = json.load(fh)
except (OSError, ValueError):
    sys.exit(0)
if not isinstance(items, list) or "andrey-session-profiles.desktop" in items:
    sys.exit(0)
anchor = "andrey-share.desktop" if "andrey-share.desktop" in items else "andrey-code.desktop"
pos = items.index(anchor) if anchor in items else 0
items.insert(pos, "andrey-session-profiles.desktop")
fd, tmp = tempfile.mkstemp(dir=os.path.dirname(path))
with os.fdopen(fd, "w", encoding="utf-8") as fh:
    json.dump(items, fh, ensure_ascii=False)
os.replace(tmp, path)
PY
fi
andrey-keys --apply >/dev/null 2>&1 || true
exit 0
