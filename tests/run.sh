#!/bin/bash
# Verificacoes rapidas, sem root e sem rede:  ./tests/run.sh
#   - sintaxe de todos os scripts (bash/sh/python)   - shellcheck (se instalado)
#   - UI C++ compila sem avisos e gera wallpaper/icones   - testes unitarios
#   - o .deb sai com as permissoes certas e sem lixo (__pycache__)
set -euo pipefail
cd "$(dirname "$0")/.."
fail=0
ok()  { echo "  ok   $*"; }
bad() { echo "  FALHA $*" >&2; fail=1; }
WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT

echo "== sintaxe dos scripts"
while IFS= read -r f; do
    case "$(head -c 64 "$f" | head -1)" in
        '#!/bin/bash'*)               bash -n "$f" && ok "$f" || bad "$f" ;;
        '#!/bin/sh'*)                 sh -n "$f"   && ok "$f" || bad "$f" ;;
        '#!/usr/bin/python3'*)        python3 -I -c 'import ast,sys; ast.parse(open(sys.argv[1], encoding="utf-8").read())' "$f" \
                                          && ok "$f" || bad "$f" ;;
    esac
done < <(git ls-files 2>/dev/null || find . -type f -not -path './.git/*' | sed 's|^\./||')
for f in pkg/andrey-os-base/root/usr/lib/andrey-os/*.py tests/*.py; do
    python3 -I -c 'import ast,sys; ast.parse(open(sys.argv[1], encoding="utf-8").read())' "$f" && ok "$f" || bad "$f"
done

if command -v shellcheck >/dev/null 2>&1; then
    echo "== shellcheck"
    # SC2086: variavel sem aspas intencional (lista de arquivos em build-deb.sh); SC1091: arquivos externos
    # (o $(pkg-config ...) sem aspas do render-assets.sh tem o proprio '# shellcheck disable=SC2046')
    shellcheck -S warning -e SC2086,SC1091 build.sh pkg/*.sh auto/* config/hooks/live/*.chroot \
        config/includes.chroot/usr/local/sbin/andrey-firstboot pkg/andrey-os-base/postinst \
        pkg/andrey-os-base/root/usr/lib/andrey-os/{apply-update,apt-helper,drivers-helper,session-setup,sync-version,user-sync} \
        pkg/andrey-os-base/root/usr/bin/{andrey-ai,andrey-lock,andrey-update,andrey-start-toggle,andrey-screenshot,andrey-volume} \
        && ok "shellcheck" || bad "shellcheck"
else
    echo "== shellcheck nao instalado (pulando)"
fi

echo "== UI em C++"
if command -v g++ >/dev/null 2>&1 && pkg-config --exists cairo x11; then
    g++ -std=c++17 -O2 -Wall -Wextra -Werror ui/andrey_os.cpp -o "$WORK/andrey_os" $(pkg-config --cflags --libs cairo x11) \
        && ok "compila sem avisos" || bad "compilacao"
    "$WORK/andrey_os" --bg "$WORK/bg.png" >/dev/null && [ -s "$WORK/bg.png" ] && ok "wallpaper" || bad "wallpaper"
    "$WORK/andrey_os" --icons "$WORK/icons/a/b" >/dev/null && [ "$(find "$WORK/icons" -name '*.svg' | wc -l)" -ge 14 ] \
        && ok "icones (14 SVGs, pasta aninhada)" || bad "icones"
    if "$WORK/andrey_os" --bg /nao/existe/x.png >/dev/null 2>&1; then bad "erro de gravacao nao devolve codigo de saida"; else ok "erro de gravacao falha com codigo != 0"; fi
else
    echo "  (sem g++/cairo/x11: pulando)"
fi

echo "== testes unitarios"
python3 -I -B -m unittest discover -s tests >/dev/null 2>"$WORK/ut.log" && ok "unittest" || { cat "$WORK/ut.log" >&2; bad "unittest"; }

echo "== pacote .deb"
if command -v dpkg-deb >/dev/null 2>&1; then
    bash pkg/build-deb.sh 0.0.0 "$WORK/deb" >/dev/null && ok "build-deb.sh" || bad "build-deb.sh"
    DEB="$WORK/deb/andrey-os-base_0.0.0_all.deb"
    listing="$(dpkg-deb -c "$DEB")"
    if grep -q '__pycache__\|\.pyc' <<<"$listing"; then bad "cache do Python dentro do .deb"; else ok "sem __pycache__"; fi
    for f in usr/bin/andrey-start usr/bin/andrey-volume usr/bin/andrey-drivers usr/lib/andrey-os/apt-helper usr/lib/andrey-os/drivers-helper usr/lib/andrey-os/session-setup; do
        grep -Eq "^-rwxr-xr-x .* \./$f\$" <<<"$listing" && ok "755 $f" || bad "$f sem permissao de execucao"
    done
    for f in usr/lib/andrey-os/andrey_search.py usr/lib/andrey-os/andrey_x.py usr/lib/andrey-os/andrey_drivers.py etc/skel/.conkyrc; do
        grep -Eq "^-rw-r--r-- .* \./$f\$" <<<"$listing" && ok "644 $f" || bad "$f com permissao errada"
    done
else
    echo "  (sem dpkg-deb: pulando)"
fi

[ "$fail" -eq 0 ] && echo "TUDO OK" || { echo "HA FALHAS" >&2; exit 1; }
