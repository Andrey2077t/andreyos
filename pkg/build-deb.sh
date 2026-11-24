#!/bin/bash
# Monta o pacote cisma-os-base a partir de pkg/cisma-os-base/.
#   uso: pkg/build-deb.sh VERSAO [PASTA_DE_SAIDA]      ex.: pkg/build-deb.sh 0.0.6 out
# Variaveis opcionais:
#   GITHUB_REPOSITORY   "dono/repo" (o GitHub Actions ja define)   padrao: Andrey2077t/cismaos
#   CISMA_UPDATE_URL   endereco base das atualizacoes (substitui o padrao do GitHub Pages)
set -euo pipefail
cd "$(dirname "$0")/.."

VERSION="${1:?uso: pkg/build-deb.sh VERSAO [SAIDA]}"
OUT="${2:-.}"
[[ "$VERSION" =~ ^[0-9]+(\.[0-9]+)*$ ]] || { echo "Versao invalida: '$VERSION' (use algo como 0.0.6)" >&2; exit 1; }

REPO="${GITHUB_REPOSITORY:-Andrey2077t/cismaos}"
OWNER="${REPO%%/*}"; NAME="${REPO##*/}"
BASE_URL="${CISMA_UPDATE_URL:-https://${OWNER,,}.github.io/${NAME}}"
BASE_URL="${BASE_URL%/}"

SRC=pkg/cisma-os-base
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

cp -a "$SRC/root/." "$STAGE/"
find "$STAGE" \( -name __pycache__ -o -name '*.pyc' \) -prune -exec rm -rf {} +   # nunca embarcar cache do Python
mkdir -p "$STAGE/DEBIAN" "$STAGE/etc/apt/sources.list.d" "$STAGE/usr/share/cisma-os" "$STAGE/usr/share/keyrings" "$STAGE/etc/skel/.config/cisma-os"

# endereco de atualizacao + versao com que novos usuarios ja nascem
printf '%s\n' "$BASE_URL" > "$STAGE/usr/share/cisma-os/update-url"
printf '%s\n' "$VERSION" > "$STAGE/etc/skel/.config/cisma-os/applied"

# fonte apt: assinada (se houver chave publica em pkg/keyring) ou, sem chave, confiada por HTTPS
if [ -f pkg/keyring/cisma-os.gpg ]; then
    cp pkg/keyring/cisma-os.gpg "$STAGE/usr/share/keyrings/cisma-os.gpg"
    echo "deb [signed-by=/usr/share/keyrings/cisma-os.gpg] ${BASE_URL}/apt ./" > "$STAGE/etc/apt/sources.list.d/cisma-os.list"
    echo "==> repositorio ASSINADO (chave em pkg/keyring/cisma-os.gpg)"
else
    rmdir "$STAGE/usr/share/keyrings" 2>/dev/null || true
    echo "deb [trusted=yes] ${BASE_URL}/apt ./" > "$STAGE/etc/apt/sources.list.d/cisma-os.list"
    echo "==> repositorio SEM assinatura (trusted=yes). Veja o README para assinar."
fi

# Janela "Desinstalar Programas" (Qt6): compilada aqui. Por isso o pacote deixa de ser "all" e passa a ser da
# arquitetura de quem compila (amd64). Sem Qt so da para seguir com CISMA_SKIP_QT=1 (testes; pacote sem a janela).
ARCH=all; QTDEPS=""
if command -v g++ >/dev/null 2>&1 && pkg-config --exists Qt6Widgets Qt6Svg Qt6Network 2>/dev/null; then
    echo "==> Compilando cisma-uninstall (Qt6)"
    # shellcheck disable=SC2046
    g++ -std=c++17 -O2 -fPIC -Wall -Wextra ui/cisma_uninstall.cpp -o "$STAGE/usr/bin/cisma-uninstall" \
        $(pkg-config --cflags --libs Qt6Widgets Qt6Svg)
    strip "$STAGE/usr/bin/cisma-uninstall" 2>/dev/null || true
    # Display de volume (aviso na tela das teclas de volume); usa Qt6Network para ser instancia unica
    echo "==> Compilando cisma-volume-osd (Qt6)"
    # shellcheck disable=SC2046
    g++ -std=c++17 -O2 -fPIC -Wall -Wextra ui/cisma_volume_osd.cpp -o "$STAGE/usr/bin/cisma-volume-osd" \
        $(pkg-config --cflags --libs Qt6Widgets Qt6Network)
    strip "$STAGE/usr/bin/cisma-volume-osd" 2>/dev/null || true
    # Cisma OSD (painel de desempenho em tempo real; sensores lidos de /proc e /sys; -pthread: thread do nvidia-smi)
    echo "==> Compilando cisma-osd (Qt6)"
    # shellcheck disable=SC2046
    g++ -std=c++17 -O2 -fPIC -pthread -Wall -Wextra ui/cisma_osd.cpp -o "$STAGE/usr/bin/cisma-osd" \
        $(pkg-config --cflags --libs Qt6Widgets Qt6Network)
    strip "$STAGE/usr/bin/cisma-osd" 2>/dev/null || true
    # Cisma Code (editor de codigo, Qt6 Widgets; usa Qt6Network para ser instancia unica)
    echo "==> Compilando cisma-code (Qt6)"
    # shellcheck disable=SC2046
    g++ -std=c++17 -O2 -fPIC -Wall -Wextra ui/cisma_code.cpp -o "$STAGE/usr/bin/cisma-code" \
        $(pkg-config --cflags --libs Qt6Widgets Qt6Network)
    strip "$STAGE/usr/bin/cisma-code" 2>/dev/null || true
    echo "==> Compilando cisma-taskbar (Qt6)"
    # shellcheck disable=SC2046
    g++ -std=c++17 -O2 -fPIC -Wall -Wextra ui/cisma_taskbar.cpp -o "$STAGE/usr/bin/cisma-taskbar" \
        $(pkg-config --cflags --libs Qt6Widgets)
    strip "$STAGE/usr/bin/cisma-taskbar" 2>/dev/null || true
    echo "==> Compilando cisma-pulse (Qt6/X11)"
    # shellcheck disable=SC2046
    g++ -std=c++17 -O2 -fPIC -Wall -Wextra ui/cisma_pulse.cpp -o "$STAGE/usr/bin/cisma-pulse" \
        $(pkg-config --cflags --libs Qt6Widgets x11)
    strip "$STAGE/usr/bin/cisma-pulse" 2>/dev/null || true
    ARCH="$(dpkg --print-architecture)"
    # t64: no Debian 13 algumas bibliotecas Qt6 ganharam esse sufixo; a alternativa deixa o apt escolher a que existir
    QTDEPS=", libqt6widgets6t64 | libqt6widgets6, libqt6svg6t64 | libqt6svg6, libqt6network6t64 | libqt6network6, qt6-qpa-plugins, libxcb-cursor0"
elif [ "${CISMA_SKIP_QT:-0}" = 1 ]; then
    echo "AVISO: sem Qt6 (qt6-base-dev qt6-svg-dev): pacote sem as janelas Qt (desinstalador, editor e barras). So para testes." >&2
    rm -f "$STAGE/usr/share/applications/cisma-uninstall.desktop" "$STAGE/usr/share/applications/cisma-code.desktop" \
          "$STAGE/usr/share/applications/cisma-taskbar.desktop" \
          "$STAGE/usr/share/applications/cisma-osd.desktop"
else
    echo "ERRO: faltam g++ e Qt6 para compilar o cisma-uninstall. Instale: qt6-base-dev qt6-svg-dev pkg-config" >&2
    exit 1
fi

# permissoes explicitas (zip/Windows perdem o bit de execucao): tudo 644/755, e so os programas sao 755
EXEC_LIB="apply-update user-sync sync-version session-setup apt-helper drivers-helper bluetooth-helper vfio-helper cisma-taskbar-mode"
find "$STAGE" -type d -exec chmod 755 {} +
find "$STAGE" -type f -exec chmod 644 {} +
chmod 755 "$STAGE"/usr/bin/cisma-*
for f in $EXEC_LIB; do chmod 755 "$STAGE/usr/lib/cisma-os/$f"; done

SIZE=$(du -sk --exclude=DEBIAN "$STAGE" | cut -f1)
# (substituicao em bash e nao em sed: as dependencias do Qt tem "|" e ","; nenhum valor aqui tem "&")
CONTROL="$(<"$SRC/control")"
CONTROL="${CONTROL//@VERSION@/$VERSION}"; CONTROL="${CONTROL//@SIZE@/$SIZE}"
CONTROL="${CONTROL//@ARCH@/$ARCH}"; CONTROL="${CONTROL//@QTDEPS@/$QTDEPS}"
printf '%s\n' "$CONTROL" > "$STAGE/DEBIAN/control"
install -m 755 "$SRC/postinst" "$STAGE/DEBIAN/postinst"

mkdir -p "$OUT"
DEB="$OUT/cisma-os-base_${VERSION}_${ARCH}.deb"
dpkg-deb --root-owner-group --build "$STAGE" "$DEB" >/dev/null

# Pacote de transicao: permite que o atualizador das instalacoes antigas (que
# ainda solicita andrey-os-base) instale o novo cisma-os-base nessa mesma atualizacao.
LEGACY="$OUT/andrey-os-base_${VERSION}_all.deb"
LEGACY_STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE" "$LEGACY_STAGE"' EXIT
mkdir -p "$LEGACY_STAGE/DEBIAN"
cat > "$LEGACY_STAGE/DEBIAN/control" <<EOF
Package: andrey-os-base
Version: $VERSION
Section: misc
Priority: optional
Architecture: all
Depends: cisma-os-base (= $VERSION)
Description: Pacote de transicao para Cisma OS
 Este pacote vazio transfere atualizacoes antigas para cisma-os-base.
EOF
dpkg-deb --root-owner-group --build "$LEGACY_STAGE" "$LEGACY" >/dev/null
echo "==> Gerados: $DEB e $LEGACY  (atualizacoes em: $BASE_URL)"
