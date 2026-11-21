#!/bin/bash
# 0.4.10: adiciona o Cisma Share ao Menu Iniciar de quem ja mantinha uma lista personalizada.
# Preserva os outros apps e a ordem escolhida; a lista padrao ja inclui o app.
F="${XDG_CONFIG_HOME:-$HOME/.config}/cisma-os/start/pinned.json"
[ -f "$F" ] || exit 0
python3 - "$F" <<'PY' || true
import json, os, sys, tempfile
path = sys.argv[1]
try:
    with open(path, encoding="utf-8") as fh:
        items = json.load(fh)
except (OSError, ValueError):
    sys.exit(0)
if not isinstance(items, list) or "cisma-share.desktop" in items:
    sys.exit(0)
if "cisma-code.desktop" in items:
    pos = items.index("cisma-code.desktop")
elif "cisma-music.desktop" in items:
    pos = items.index("cisma-music.desktop") + 1
elif "cisma-ai.desktop" in items:
    pos = items.index("cisma-ai.desktop") + 1
else:
    pos = 0
items.insert(pos, "cisma-share.desktop")
fd, tmp = tempfile.mkstemp(dir=os.path.dirname(path))
with os.fdopen(fd, "w", encoding="utf-8") as fh:
    json.dump(items, fh, ensure_ascii=False)
os.replace(tmp, path)
PY
exit 0
