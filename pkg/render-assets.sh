#!/bin/bash
# Recompila a UI em C++/Cairo e regenera wallpaper, tela de boot e icones SVG
# dentro de pkg/andrey-os-base/root. Precisa de: g++ pkg-config libcairo2-dev libx11-dev fonts-inter
set -euo pipefail
cd "$(dirname "$0")/.."
R=pkg/andrey-os-base/root
BIN="$(mktemp -d)/andrey_os"

g++ -std=c++17 -O2 ui/andrey_os.cpp -o "$BIN" $(pkg-config --cflags --libs cairo x11)

mkdir -p "$R/usr/share/backgrounds/andrey" "$R/usr/share/plymouth/themes/andrey"
"$BIN" --bg "$R/usr/share/backgrounds/andrey/andrey-os.png"
cp -f "$R/usr/share/backgrounds/andrey/andrey-os.png" "$R/usr/share/plymouth/themes/andrey/bg.png"

TMP="$(mktemp -d)"
"$BIN" --icons "$TMP" >/dev/null
mkdir -p "$R/usr/share/icons/Andrey/scalable"
cp -rf "$TMP/." "$R/usr/share/icons/Andrey/scalable/"
rm -rf "$TMP"
echo "==> Arte regenerada em $R"
