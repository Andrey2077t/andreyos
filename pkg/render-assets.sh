#!/bin/bash
# Recompila a UI em C++/Cairo e copia o wallpaper aprovado para a tela de boot e regenera icones SVG
# dentro de pkg/cisma-os-base/root. Precisa de: g++ pkg-config libcairo2-dev libx11-dev fonts-inter
# O logo do wallpaper usa a fonte Poppins; sem ela o desenho sairia diferente do aprovado, entao
# nesse caso o wallpaper atual e MANTIDO (so os icones, que nao usam texto, sao regenerados).
set -euo pipefail
cd "$(dirname "$0")/.."
R=pkg/cisma-os-base/root
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# shellcheck disable=SC2046   # pkg-config devolve varios -I/-l separados por espaco: sem aspas e intencional
g++ -std=c++17 -O2 ui/cisma_os.cpp -o "$WORK/cisma_os" $(pkg-config --cflags --libs cairo x11)

BG="$R/usr/share/backgrounds/cisma/cisma-os.png"
PLYMOUTH="$R/usr/share/plymouth/themes/cisma"
if [ ! -s "$BG" ]; then
    echo "ERRO: wallpaper Cisma OS ausente em $BG." >&2
    exit 1
fi
mkdir -p "$PLYMOUTH"
cp -f "$BG" "$PLYMOUTH/bg.png"

"$WORK/cisma_os" --icons "$WORK/icons" >/dev/null
mkdir -p "$R/usr/share/icons/Cisma/scalable"
cp -rf "$WORK/icons/." "$R/usr/share/icons/Cisma/scalable/"
echo "==> Arte regenerada em $R"
