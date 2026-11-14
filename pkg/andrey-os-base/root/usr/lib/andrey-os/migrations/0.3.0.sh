#!/bin/bash
# 0.3.0: coloca o Andrey Music entre os aplicativos fixados do Menu Iniciar de quem ja tinha uma lista salva
# (quem nunca mexeu nos fixados ja recebe o padrao novo, que inclui o Andrey Music). Idempotente.
F="${XDG_CONFIG_HOME:-$HOME/.config}/andrey-os/start/pinned.json"
[ -f "$F" ] || exit 0
python3 - "$F" <<'PY' || true
import json, os, sys, tempfile
f = sys.argv[1]
try:
    with open(f, encoding="utf-8") as fh:
        data = json.load(fh)
except (OSError, ValueError):
    sys.exit(0)
if not isinstance(data, list) or "andrey-music.desktop" in data:
    sys.exit(0)
# logo depois do Andrey AI (ou no comeco, se a pessoa tirou o Andrey AI)
pos = data.index("andrey-ai.desktop") + 1 if "andrey-ai.desktop" in data else 0
data.insert(pos, "andrey-music.desktop")
fd, tmp = tempfile.mkstemp(dir=os.path.dirname(f))
with os.fdopen(fd, "w", encoding="utf-8") as fh:
    json.dump(data, fh, ensure_ascii=False)
os.replace(tmp, f)
PY
exit 0
