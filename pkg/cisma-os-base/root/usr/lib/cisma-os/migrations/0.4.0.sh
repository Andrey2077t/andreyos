#!/bin/bash
# 0.4.0: dock transparente. Quem ja tinha o ~/.config/gtk-3.0/gtk.css antigo tem la a regra que pinta TODOS os paineis
# de escuro (isso cobriria o dock transparente); tira so o PanelToplevel/#PanelWidget dessa regra e mantem o resto do
# arquivo (a pessoa pode ter personalizado). O fundo em si (barra escura, dock transparente) vem do session-setup
# (cisma-dock --apply-style, nivel 9). Idempotente.
F="${XDG_CONFIG_HOME:-$HOME/.config}/gtk-3.0/gtk.css"
[ -f "$F" ] || exit 0
python3 - "$F" <<'PY' || true
import os, sys, tempfile
f = sys.argv[1]
try:
    s = open(f, encoding="utf-8").read()
except OSError:
    sys.exit(0)
old = ".mate-panel-menu-bar menubar,\nPanelToplevel,\n#PanelWidget {"
new = ".mate-panel-menu-bar menubar {"
if old not in s:
    sys.exit(0)
fd, tmp = tempfile.mkstemp(dir=os.path.dirname(f))
with os.fdopen(fd, "w", encoding="utf-8") as fh:
    fh.write(s.replace(old, new))
os.replace(tmp, f)
PY
exit 0
