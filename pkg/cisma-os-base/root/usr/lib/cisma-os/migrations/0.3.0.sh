#!/bin/bash
# 0.3.0: coloca o Cisma Music e o Cisma Code entre os aplicativos fixados do Menu Iniciar de quem ja tinha uma
# lista salva (quem nunca mexeu nos fixados ja recebe o padrao novo, que inclui os dois). Idempotente.
F="${XDG_CONFIG_HOME:-$HOME/.config}/cisma-os/start/pinned.json"
[ -f "$F" ] || exit 0
python3 - "$F" <<'PY' || true
import json, os, sys, tempfile
f = sys.argv[1]
try:
    with open(f, encoding="utf-8") as fh:
        data = json.load(fh)
except (OSError, ValueError):
    sys.exit(0)
if not isinstance(data, list):
    sys.exit(0)
changed = False
# logo depois do Cisma AI (ou no comeco, se a pessoa tirou o Cisma AI); o Cisma Code vem depois do Cisma Music
for app in ("cisma-music.desktop", "cisma-code.desktop"):
    if app in data:
        continue
    if app == "cisma-code.desktop" and "cisma-music.desktop" in data:
        pos = data.index("cisma-music.desktop") + 1
    else:
        pos = data.index("cisma-ai.desktop") + 1 if "cisma-ai.desktop" in data else 0
    data.insert(pos, app)
    changed = True
if not changed:
    sys.exit(0)
fd, tmp = tempfile.mkstemp(dir=os.path.dirname(f))
with os.fdopen(fd, "w", encoding="utf-8") as fh:
    json.dump(data, fh, ensure_ascii=False)
os.replace(tmp, f)
PY
exit 0
