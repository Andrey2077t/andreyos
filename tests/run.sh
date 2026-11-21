#!/bin/bash
# Verificacoes rapidas, sem root e sem rede:  ./tests/run.sh
#   - sintaxe de todos os scripts (bash/sh/python)   - shellcheck (se instalado)
#   - UI C++ compila sem avisos e copia wallpaper/icones   - testes unitarios
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
for f in pkg/cisma-os-base/root/usr/lib/cisma-os/*.py tests/*.py; do
    python3 -I -c 'import ast,sys; ast.parse(open(sys.argv[1], encoding="utf-8").read())' "$f" && ok "$f" || bad "$f"
done

if command -v shellcheck >/dev/null 2>&1; then
    echo "== shellcheck"
    # SC2086: variavel sem aspas intencional (lista de arquivos em build-deb.sh); SC1091: arquivos externos
    # (o $(pkg-config ...) sem aspas do render-assets.sh tem o proprio '# shellcheck disable=SC2046')
    shellcheck -S warning -e SC2086,SC1091 build.sh pkg/*.sh auto/* config/hooks/live/*.chroot \
        config/includes.chroot/usr/local/sbin/cisma-firstboot pkg/cisma-os-base/postinst \
        pkg/cisma-os-base/root/usr/lib/cisma-os/{apply-update,apt-helper,bluetooth-helper,drivers-helper,session-setup,sync-version,user-sync} \
        pkg/cisma-os-base/root/usr/bin/{cisma-ai,cisma-music,cisma-bluetooth,cisma-lock,cisma-update,cisma-start-toggle,cisma-screenshot,cisma-volume} \
        && ok "shellcheck" || bad "shellcheck"
else
    echo "== shellcheck nao instalado (pulando)"
fi

echo "== UI em C++"
if command -v g++ >/dev/null 2>&1 && pkg-config --exists cairo x11; then
    g++ -std=c++17 -O2 -Wall -Wextra -Werror ui/cisma_os.cpp -o "$WORK/cisma_os" $(pkg-config --cflags --libs cairo x11) \
        && ok "compila sem avisos" || bad "compilacao"
    "$WORK/cisma_os" --bg "$WORK/bg.png" >/dev/null && [ -s "$WORK/bg.png" ] && ok "wallpaper" || bad "wallpaper"
    "$WORK/cisma_os" --icons "$WORK/icons/a/b" >/dev/null && [ "$(find "$WORK/icons" -name '*.svg' | wc -l)" -ge 14 ] \
        && ok "icones (14 SVGs, pasta aninhada)" || bad "icones"
    if "$WORK/cisma_os" --bg /nao/existe/x.png >/dev/null 2>&1; then bad "erro de gravacao nao devolve codigo de saida"; else ok "erro de gravacao falha com codigo != 0"; fi
else
    echo "  (sem g++/cairo/x11: pulando)"
fi

echo "== Desinstalar Programas (Qt6)"
HAVE_QT=0
if command -v g++ >/dev/null 2>&1 && pkg-config --exists Qt6Widgets Qt6Svg 2>/dev/null; then
    HAVE_QT=1
    g++ -std=c++17 -O2 -fPIC -Wall -Wextra ui/cisma_uninstall.cpp -o "$WORK/cisma-uninstall" \
        $(pkg-config --cflags --libs Qt6Widgets Qt6Svg) && ok "cisma_uninstall.cpp compila" || bad "cisma_uninstall.cpp nao compila"
else
    echo "  (sem g++/Qt6: pulando a compilacao; instale qt6-base-dev qt6-svg-dev)"
fi

echo "== Display de volume (Qt6)"
if [ "$HAVE_QT" = 1 ] && pkg-config --exists Qt6Network 2>/dev/null; then
    g++ -std=c++17 -O2 -fPIC -Wall -Wextra -Werror ui/cisma_volume_osd.cpp -o "$WORK/cisma-volume-osd" \
        $(pkg-config --cflags --libs Qt6Widgets Qt6Network) && ok "cisma_volume_osd.cpp compila sem avisos" \
        || bad "cisma_volume_osd.cpp nao compila"
    # desenha o aviso sem tela (plugin offscreen); se o Qt desta maquina nao tiver o plugin, so avisa
    if QT_QPA_PLATFORM=offscreen "$WORK/cisma-volume-osd" --grab "$WORK/osd70.png" 70 0 2>/dev/null; then
        [ -s "$WORK/osd70.png" ] && ok "desenho do aviso (70%)" || bad "aviso 70% sem imagem"
        QT_QPA_PLATFORM=offscreen "$WORK/cisma-volume-osd" --grab "$WORK/osd0.png" 40 1 2>/dev/null \
            && [ -s "$WORK/osd0.png" ] && ok "desenho do aviso (mudo)" || bad "aviso mudo sem imagem"
    else
        echo "  (plugin offscreen do Qt indisponivel: pulando o desenho)"
    fi
else
    echo "  (sem g++/Qt6Widgets/Qt6Network: pulando)"
fi

echo "== Cisma Code (Qt6)"
if [ "$HAVE_QT" = 1 ] && pkg-config --exists Qt6Network 2>/dev/null; then
    g++ -std=c++17 -O2 -fPIC -Wall -Wextra -Werror ui/cisma_code.cpp -o "$WORK/cisma-code" \
        $(pkg-config --cflags --libs Qt6Widgets Qt6Network) && ok "cisma_code.cpp compila sem avisos" || bad "cisma_code.cpp nao compila"
    [ "$("$WORK/cisma-code" --version)" = "Cisma Code $(grep -o 'APP_VERSION = "[^"]*"' ui/cisma_code.cpp | cut -d'"' -f2)" ] \
        && ok "--version" || bad "--version"
    # autoteste do editor e desenho da janela sem tela (se o Qt desta maquina nao tiver o plugin offscreen, so avisa)
    if QT_QPA_PLATFORM=offscreen "$WORK/cisma-code" --selftest >"$WORK/code-self.log" 2>&1; then
        grep -q '^OK:' "$WORK/code-self.log" && ok "autoteste: $(grep '^OK:' "$WORK/code-self.log")" || bad "autoteste sem resultado"
        QT_QPA_PLATFORM=offscreen HOME="$WORK/h" XDG_CONFIG_HOME="$WORK/h" "$WORK/cisma-code" --grab "$WORK/code.png" ui/cisma_code.cpp >/dev/null 2>&1 \
            && [ -s "$WORK/code.png" ] && ok "desenho da janela" || bad "janela sem imagem"
    elif grep -q 'FALHOU' "$WORK/code-self.log"; then
        cat "$WORK/code-self.log" >&2; bad "autoteste do editor"
    else
        echo "  (plugin offscreen do Qt indisponivel: pulando o autoteste)"
    fi
else
    echo "  (sem g++/Qt6Widgets/Qt6Network: pulando)"
fi

echo "== testes unitarios"
python3 -I -B -m unittest discover -s tests >/dev/null 2>"$WORK/ut.log" && ok "unittest" || { cat "$WORK/ut.log" >&2; bad "unittest"; }

echo "== pacote .deb"
if command -v dpkg-deb >/dev/null 2>&1; then
    # sem Qt o pacote sai sem a janela de desinstalar (so para testar o resto)
    CISMA_SKIP_QT=1 bash pkg/build-deb.sh 0.0.0 "$WORK/deb" >/dev/null && ok "build-deb.sh" || bad "build-deb.sh"
    DEB="$(ls "$WORK"/deb/cisma-os-base_0.0.0_*.deb)"
    listing="$(dpkg-deb -c "$DEB")"
    if [ "$HAVE_QT" = 1 ]; then
        grep -Eq "^-rwxr-xr-x .* \./usr/bin/cisma-uninstall\$" <<<"$listing" && ok "755 usr/bin/cisma-uninstall" || bad "cisma-uninstall ausente/sem permissao"
        grep -Eq "^-rw-r--r-- .* \./usr/share/applications/cisma-uninstall.desktop\$" <<<"$listing" && ok "644 cisma-uninstall.desktop" || bad "atalho do Desinstalar"
        grep -Eq "^-rwxr-xr-x .* \./usr/bin/cisma-volume-osd\$" <<<"$listing" && ok "755 usr/bin/cisma-volume-osd" || bad "cisma-volume-osd ausente/sem permissao"
        grep -q 'libqt6widgets6t64 | libqt6widgets6' <(dpkg-deb -f "$DEB" Depends) && ok "Depends com as bibliotecas Qt6" || bad "Depends sem Qt6"
        grep -q 'libqt6network6t64 | libqt6network6' <(dpkg-deb -f "$DEB" Depends) && ok "Depends com Qt6Network" || bad "Depends sem Qt6Network"
        grep -Eq "^-rwxr-xr-x .* \./usr/bin/cisma-code\$" <<<"$listing" && ok "755 usr/bin/cisma-code" || bad "cisma-code ausente/sem permissao"
        grep -Eq "^-rw-r--r-- .* \./usr/share/applications/cisma-code.desktop\$" <<<"$listing" && ok "644 cisma-code.desktop" || bad "atalho do Cisma Code"
    else
        grep -q 'cisma-uninstall.desktop' <<<"$listing" && bad "atalho do Desinstalar sem o programa" || ok "sem atalho orfao do Desinstalar"
        grep -q 'cisma-code.desktop' <<<"$listing" && bad "atalho do Cisma Code sem o programa" || ok "sem atalho orfao do Cisma Code"
    fi
    for d in engrampa zip unzip; do
        grep -Eq "(^|[ ,])$d([ ,]|\$)" <(dpkg-deb -f "$DEB" Depends) && ok "Depends tem $d" || bad "Depends sem $d (compactar/extrair)"
    done
    if grep -q '__pycache__\|\.pyc' <<<"$listing"; then bad "cache do Python dentro do .deb"; else ok "sem __pycache__"; fi
    for f in usr/bin/cisma-start usr/bin/cisma-share usr/bin/cisma-session-profiles usr/bin/cisma-audio-heal usr/bin/cisma-mouse usr/bin/cisma-music usr/bin/cisma-ai usr/bin/cisma-volume usr/bin/cisma-tabs usr/bin/cisma-drivers usr/bin/cisma-bluetooth usr/bin/cisma-games usr/bin/cisma-exe usr/bin/cisma-dock usr/lib/cisma-os/apt-helper usr/lib/cisma-os/drivers-helper usr/lib/cisma-os/bluetooth-helper usr/lib/cisma-os/session-setup; do
        grep -Eq "^-rwxr-xr-x .* \./$f\$" <<<"$listing" && ok "755 $f" || bad "$f sem permissao de execucao"
    done
    for f in usr/lib/cisma-os/cisma_search.py usr/lib/cisma-os/cisma_x.py usr/lib/cisma-os/cisma_drivers.py usr/lib/cisma-os/cisma_games.py usr/lib/cisma-os/cisma_exe.py usr/share/applications/cisma-exe.desktop usr/share/applications/cisma-dock.desktop usr/share/applications/cisma-mouse.desktop usr/share/applications/cisma-exe-shortcut.desktop etc/xdg/mimeapps.list usr/lib/cisma-os/cisma_uninstall.py usr/lib/sysctl.d/80-cisma-games.conf usr/lib/modprobe.d/cisma-bluetooth.conf usr/lib/udev/rules.d/80-cisma-bluetooth.rules etc/pulse/default.pa.d/50-cisma-hotplug.pa usr/share/applications/cisma-audio-refresh.desktop usr/share/applications/cisma-share.desktop usr/share/applications/cisma-session-profiles.desktop usr/share/applications/cisma-bluetooth.desktop usr/share/applications/cisma-music.desktop usr/share/applications/cisma-tabs-group.desktop etc/xdg/autostart/cisma-tabs.desktop etc/xdg/autostart/cisma-audio-heal.desktop etc/xdg/autostart/cisma-mouse.desktop usr/share/icons/Cisma/scalable/apps/cisma-music.svg usr/share/icons/Cisma/scalable/apps/cisma-code.svg usr/share/icons/Cisma/scalable/apps/cisma-share.svg usr/share/icons/Cisma/scalable/apps/cisma-session-profiles.svg usr/share/backgrounds/cisma/cisma-os-focus.png etc/skel/.conkyrc; do
        grep -Eq "^-rw-r--r-- .* \./$f\$" <<<"$listing" && ok "644 $f" || bad "$f com permissao errada"
    done
else
    echo "  (sem dpkg-deb: pulando)"
fi

[ "$fail" -eq 0 ] && echo "TUDO OK" || { echo "HA FALHAS" >&2; exit 1; }
