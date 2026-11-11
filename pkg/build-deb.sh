#!/bin/bash
# Monta o pacote andrey-os-base a partir de pkg/andrey-os-base/.
#   uso: pkg/build-deb.sh VERSAO [PASTA_DE_SAIDA]      ex.: pkg/build-deb.sh 0.0.6 out
# Variaveis opcionais:
#   GITHUB_REPOSITORY   "dono/repo" (o GitHub Actions ja define)   padrao: Andrey2077t/andreyos
#   ANDREY_UPDATE_URL   endereco base das atualizacoes (substitui o padrao do GitHub Pages)
set -euo pipefail
cd "$(dirname "$0")/.."

VERSION="${1:?uso: pkg/build-deb.sh VERSAO [SAIDA]}"
OUT="${2:-.}"
[[ "$VERSION" =~ ^[0-9]+(\.[0-9]+)*$ ]] || { echo "Versao invalida: '$VERSION' (use algo como 0.0.6)" >&2; exit 1; }

REPO="${GITHUB_REPOSITORY:-Andrey2077t/andreyos}"
OWNER="${REPO%%/*}"; NAME="${REPO##*/}"
BASE_URL="${ANDREY_UPDATE_URL:-https://${OWNER,,}.github.io/${NAME}}"
BASE_URL="${BASE_URL%/}"

SRC=pkg/andrey-os-base
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

cp -a "$SRC/root/." "$STAGE/"
mkdir -p "$STAGE/DEBIAN" "$STAGE/etc/apt/sources.list.d" "$STAGE/usr/share/andrey-os" "$STAGE/usr/share/keyrings" "$STAGE/etc/skel/.config/andrey-os"

# endereco de atualizacao + versao com que novos usuarios ja nascem
printf '%s\n' "$BASE_URL" > "$STAGE/usr/share/andrey-os/update-url"
printf '%s\n' "$VERSION" > "$STAGE/etc/skel/.config/andrey-os/applied"

# fonte apt: assinada (se houver chave publica em pkg/keyring) ou, sem chave, confiada por HTTPS
if [ -f pkg/keyring/andrey-os.gpg ]; then
    cp pkg/keyring/andrey-os.gpg "$STAGE/usr/share/keyrings/andrey-os.gpg"
    echo "deb [signed-by=/usr/share/keyrings/andrey-os.gpg] ${BASE_URL}/apt ./" > "$STAGE/etc/apt/sources.list.d/andrey-os.list"
    echo "==> repositorio ASSINADO (chave em pkg/keyring/andrey-os.gpg)"
else
    rmdir "$STAGE/usr/share/keyrings" 2>/dev/null || true
    echo "deb [trusted=yes] ${BASE_URL}/apt ./" > "$STAGE/etc/apt/sources.list.d/andrey-os.list"
    echo "==> repositorio SEM assinatura (trusted=yes). Veja o README para assinar."
fi

# permissoes explicitas (zip/Windows perdem o bit de execucao)
chmod 755 "$STAGE/usr/bin/andrey-update" "$STAGE/usr/lib/andrey-os/apply-update" "$STAGE/usr/lib/andrey-os/user-sync"
find "$STAGE" -type d -exec chmod 755 {} +
find "$STAGE" -type f ! -path "$STAGE/usr/bin/*" ! -path "$STAGE/usr/lib/andrey-os/apply-update" ! -path "$STAGE/usr/lib/andrey-os/user-sync" -exec chmod 644 {} +

SIZE=$(du -sk --exclude=DEBIAN "$STAGE" | cut -f1)
sed "s/@VERSION@/${VERSION}/; s/@SIZE@/${SIZE}/" "$SRC/control" > "$STAGE/DEBIAN/control"
install -m 755 "$SRC/postinst" "$STAGE/DEBIAN/postinst"

mkdir -p "$OUT"
DEB="$OUT/andrey-os-base_${VERSION}_all.deb"
dpkg-deb --root-owner-group --build "$STAGE" "$DEB" >/dev/null
echo "==> Gerado: $DEB  (atualizacoes em: $BASE_URL)"
