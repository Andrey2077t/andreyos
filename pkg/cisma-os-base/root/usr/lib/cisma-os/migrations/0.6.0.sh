#!/bin/bash
# 0.6.0: migra caminhos e personalizacoes da marca Andrey OS para Cisma OS.
set -e

# O tema antigo deixou de ser instalado; atualiza preferencias antigas do MATE.
if command -v gsettings >/dev/null 2>&1; then
    theme=$(gsettings get org.mate.interface icon-theme 2>/dev/null || true)
    case "$theme" in *Andrey*) gsettings set org.mate.interface icon-theme 'Cisma' 2>/dev/null || true ;; esac
    wallpaper=$(gsettings get org.mate.background picture-filename 2>/dev/null || true)
    case "$wallpaper" in *backgrounds/andrey/*) gsettings set org.mate.background picture-filename '/usr/share/backgrounds/cisma/cisma-os.png' 2>/dev/null || true ;; esac
fi

# Painel guarda atalhos/painel customizado no dconf. Regrava somente seus valores,
# trocando nomes de comandos/arquivos; nao apaga nem reseta o layout do usuario.
if command -v dconf >/dev/null 2>&1; then
    dump=$(dconf dump /org/mate/panel/ 2>/dev/null || true)
    if [ -n "$dump" ]; then
        printf '%s\n' "$dump" | sed -e 's/andrey/cisma/g' -e 's/Andrey/Cisma/g' | dconf load /org/mate/panel/ 2>/dev/null || true
    fi
fi

# Migra atalhos salvos com o ID de desktop antigo.
for f in "$HOME/.config/mimeapps.list" "$HOME/.local/share/applications/mimeapps.list"; do
    [ -f "$f" ] || continue
    sed -i -e 's/andrey-/cisma-/g' -e 's/Andrey /Cisma /g' "$f"
done
