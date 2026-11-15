#!/bin/bash
# Recompila a UI em C++/Cairo e regenera wallpaper, tela de boot e icones SVG
# dentro de pkg/andrey-os-base/root. Precisa de: g++ pkg-config libcairo2-dev libx11-dev fonts-inter
# O logo do wallpaper usa a fonte Poppins; sem ela o desenho sairia diferente do aprovado, entao
# nesse caso o wallpaper atual e MANTIDO (so os icones, que nao usam texto, sao regenerados).
set -euo pipefail
cd "$(dirname "$0")/.."
R=pkg/andrey-os-base/root
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# shellcheck disable=SC2046   # pkg-config devolve varios -I/-l separados por espaco: sem aspas e intencional
g++ -std=c++17 -O2 ui/andrey_os.cpp -o "$WORK/andrey_os" $(pkg-config --cflags --libs cairo x11)

BG="$R/usr/share/backgrounds/andrey/andrey-os.png"
FONTS="$(fc-list 2>/dev/null || true)"      # (sem pipe direto no grep -q: com pipefail o SIGPIPE derrubaria o teste)
if grep -qi 'poppins' <<<"$FONTS"; then
    mkdir -p "$(dirname "$BG")" "$R/usr/share/plymouth/themes/andrey"
    "$WORK/andrey_os" --bg "$WORK/bg.png"
    cp -f "$WORK/bg.png" "$BG"
    cp -f "$WORK/bg.png" "$R/usr/share/plymouth/themes/andrey/bg.png"
elif [ -f "$BG" ]; then
    echo "AVISO: fonte Poppins ausente; mantendo o wallpaper existente (instale fonts-poppins ou a Poppins do Google Fonts para regenerar)." >&2
else
    echo "ERRO: sem a fonte Poppins e sem wallpaper pronto em $BG." >&2
    exit 1
fi

"$WORK/andrey_os" --icons "$WORK/icons" >/dev/null
mkdir -p "$R/usr/share/icons/Andrey/scalable"
cp -rf "$WORK/icons/." "$R/usr/share/icons/Andrey/scalable/"
echo "==> Arte regenerada em $R"
