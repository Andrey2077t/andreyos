#!/bin/bash
# 0.1.4: coloca o Cisma AI entre os aplicativos fixados do Menu Iniciar de quem ja tinha uma lista salva
# (quem nunca mexeu nos fixados ja recebe o padrao novo, que inclui o Cisma AI).
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
if not isinstance(data, list) or "cisma-ai.desktop" in data:
    sys.exit(0)
fd, tmp = tempfile.mkstemp(dir=os.path.dirname(f))
with os.fdopen(fd, "w", encoding="utf-8") as fh:
    json.dump(["cisma-ai.desktop"] + data, fh, ensure_ascii=False)
os.replace(tmp, f)
PY
exit 0
