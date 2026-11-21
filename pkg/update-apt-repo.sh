#!/bin/bash
# Adiciona um .deb ao repositorio apt (pasta do GitHub Pages) e regenera os indices.
#   uso: pkg/update-apt-repo.sh PASTA_PAGES ARQUIVO.deb VERSAO "NOTAS"
# Assina (InRelease/Release.gpg) se SIGN=1 e houver chave no gpg (GNUPGHOME).
set -euo pipefail

PAGES="${1:?pasta do pages}"; DEB="${2:?arquivo .deb}"; VERSION="${3:?versao}"; NOTES="${4:-}"; LEGACY_DEB="${5:-}"
APT="$PAGES/apt"
KEEP=10
mkdir -p "$APT/pool"

# nao deixa publicar versao MENOR que a atual. Igual e permitido: serve para refazer uma release
# que falhou depois de o gh-pages ja ter sido atualizado (ex.: erro 500 do GitHub ao criar a release).
PREV=""
if [ -f "$PAGES/latest.json" ]; then PREV=$(jq -r '.version // empty' "$PAGES/latest.json" || true); fi
if [ -n "$PREV" ] && dpkg --compare-versions "$VERSION" eq "$PREV"; then
    echo "AVISO: a versao $VERSION ja estava no repositorio; republicando (anterior mantida)." >&2
    PREV=$(jq -r '.previous // empty' "$PAGES/latest.json" || true)
elif [ -n "$PREV" ] && ! dpkg --compare-versions "$VERSION" gt "$PREV"; then
    echo "ERRO: a versao $VERSION e menor que a atual ($PREV)." >&2
    exit 1
fi

cp -f "$DEB" "$APT/pool/"
[ -z "$LEGACY_DEB" ] || cp -f "$LEGACY_DEB" "$APT/pool/"
# Mantem as versoes recentes dos dois nomes de pacote durante a transicao.
for pattern in cisma-os-base andrey-os-base; do
    mapfile -t old_versions < <(ls -1 "$APT"/pool/"${pattern}"_*.deb 2>/dev/null | sort -V | head -n -"$KEEP")
    [ "${#old_versions[@]}" -eq 0 ] || rm -f -- "${old_versions[@]}"
done

cd "$APT"
rm -f Packages Packages.gz Release InRelease Release.gpg
dpkg-scanpackages --multiversion pool /dev/null > Packages 2>scan.log || { cat scan.log; exit 1; }
rm -f scan.log
gzip -9 -k -f Packages
# Release (repositorio "flat"): hashes dos indices, sem depender do apt-ftparchive
{
    echo "Origin: Cisma OS"
    echo "Label: Cisma OS"
    echo "Suite: stable"
    echo "Description: Repositorio do Cisma OS"
    echo "Date: $(LC_ALL=C date -Ru)"
    echo "MD5Sum:"
    for f in Packages Packages.gz; do echo " $(md5sum "$f" | cut -d' ' -f1) $(stat -c %s "$f") $f"; done
    echo "SHA256:"
    for f in Packages Packages.gz; do echo " $(sha256sum "$f" | cut -d' ' -f1) $(stat -c %s "$f") $f"; done
} > Release

if [ "${SIGN:-0}" = "1" ]; then
    gpg --batch --yes --clearsign -o InRelease Release
    gpg --batch --yes --armor --detach-sign -o Release.gpg Release
    gpg --batch --yes --export > cisma-os.gpg
    echo "==> repositorio assinado"
fi
cd - >/dev/null

# latest.json (lido pelo cisma-update) + paginazinha
jq -n --arg v "$VERSION" --arg p "$PREV" --arg n "$NOTES" --arg d "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
    '{version:$v, previous:$p, notes:$n, date:$d}' > "$PAGES/latest.json"
touch "$PAGES/.nojekyll"
cat > "$PAGES/index.html" <<HTML
<!doctype html><meta charset="utf-8"><title>Cisma OS - atualizacoes</title>
<body style="font-family:sans-serif;background:#0b0306;color:#ededf0;padding:2rem">
<h1 style="color:#ed1226">Cisma OS</h1>
<p>Versao atual: <b>$VERSION</b>${PREV:+ (anterior: $PREV)}</p>
<p>Repositorio apt: <code>apt/</code> &middot; <a style="color:#ed1226" href="latest.json">latest.json</a></p>
</body>
HTML
echo "==> repositorio atualizado: $VERSION (anterior: ${PREV:-nenhuma})"
