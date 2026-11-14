#!/bin/bash
# 0.1.10: (1) tira o dock lateral esquerdo (fica so o dock de baixo, com o botao de desligar no fim)
#         (2) garante F2 = Renomear no Caja (Arquivos)
# Idempotente: roda uma vez por usuario.

# (2) F2 renomear: so acrescenta se a pessoa nao tiver um atalho proprio para Rename
A="${XDG_CONFIG_HOME:-$HOME/.config}/caja/accels"
mkdir -p "$(dirname "$A")"
if ! grep -q '^(gtk_accel_path "<Actions>/DirViewActions/Rename"' "$A" 2>/dev/null; then
    # tira uma eventual versao comentada/vazia do mesmo atalho e coloca F2
    [ -f "$A" ] && sed -i '/DirViewActions\/Rename"/d' "$A"
    echo '(gtk_accel_path "<Actions>/DirViewActions/Rename" "F2")' >> "$A"
fi

# (1) recarrega o layout dos paineis UMA vez (mesmo jeito do session-setup)
if pgrep -x mate-panel >/dev/null 2>&1; then
    dconf reset -f /org/mate/panel/ 2>/dev/null || true
    sleep 1
    killall mate-panel 2>/dev/null || true   # o mate-session reinicia com o layout "andrey"
fi
exit 0
