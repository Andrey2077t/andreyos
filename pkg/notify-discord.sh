#!/usr/bin/env bash
# Manda pro canal do Discord o que mudou na versao publicada.
# Uso: notify-discord.sh <versao> "<notas>"
# Precisa de: DISCORD_BOT_TOKEN, DISCORD_CHANNEL_ID, GITHUB_REPOSITORY (ja vem no Actions), jq, curl, git
set -uo pipefail

VERSION="${1:?versao}"
NOTES="${2:-}"
: "${DISCORD_BOT_TOKEN:?faltou o segredo DISCORD_BOT_TOKEN}"
: "${DISCORD_CHANNEL_ID:?faltou DISCORD_CHANNEL_ID}"

git fetch --tags --force --quiet 2>/dev/null || true
git fetch --unshallow --quiet 2>/dev/null || true

PREV="$(git tag --list 'v*' --sort=-v:refname | grep -vx "v$VERSION" | head -n1 || true)"
if [ -n "$PREV" ]; then
  RANGE="$PREV..HEAD"; DE="de \`$PREV\`"
else
  RANGE="HEAD"; DE="(primeira versão)"
fi

COMMITS="$(git log --no-merges --pretty='• %s (`%h`)' "$RANGE" 2>/dev/null | head -n 25)"
[ -z "$COMMITS" ] && COMMITS="• (nenhum commit novo)"

# arquivos: A = novo, M = modificado, D = removido, R = renomeado
FILES="$(git diff --name-status "${PREV:-4b825dc642cb9a8f1ba3a4b3a5a8b5c7f1e0e5d}" HEAD 2>/dev/null \
  | awk '{s=$1; $1=""; sub(/^ /,""); i=(s~/^A/)?"🆕":(s~/^D/)?"🗑️":(s~/^R/)?"🔀":"✏️"; print i" `"$0"`"}' | head -n 40)"
TOTAL="$(git diff --name-only "${PREV:-4b825dc642cb9a8f1ba3a4b3a5a8b5c7f1e0e5d}" HEAD 2>/dev/null | wc -l)"
STAT="$(git diff --shortstat "${PREV:-4b825dc642cb9a8f1ba3a4b3a5a8b5c7f1e0e5d}" HEAD 2>/dev/null | sed 's/^ *//')"
[ -z "$FILES" ] && FILES="(nenhum arquivo alterado)"
[ "$TOTAL" -gt 40 ] && FILES="$FILES"$'\n'"… e mais $((TOTAL-40)) arquivos"

cut_to() { printf '%s' "$1" | head -c "$2"; }

DESC="$(printf '**Notas:**\n%s\n\n**Commits:**\n%s\n\n**Arquivos (%s):**\n%s\n\n_%s_' \
  "$NOTES" "$COMMITS" "$TOTAL" "$FILES" "$STAT")"
DESC="$(cut_to "$DESC" 3900)"

PAYLOAD="$(jq -n \
  --arg title "🚀 Andrey OS $VERSION" \
  --arg desc  "$DESC" \
  --arg url   "https://github.com/${GITHUB_REPOSITORY}/releases/tag/v${VERSION}" \
  --arg foot  "Atualização $DE → v$VERSION" \
  '{embeds:[{title:$title, description:$desc, url:$url, color:5763719, footer:{text:$foot}}]}')"

for i in 1 2 3; do
  CODE="$(curl -sS -o /tmp/discord-resp.json -w '%{http_code}' -X POST \
    "https://discord.com/api/v10/channels/${DISCORD_CHANNEL_ID}/messages" \
    -H "Authorization: Bot ${DISCORD_BOT_TOKEN}" \
    -H "Content-Type: application/json" \
    -d "$PAYLOAD")"
  [ "$CODE" = 200 ] && { echo "Discord: mensagem enviada."; exit 0; }
  [ "$CODE" = 429 ] && sleep "$(jq -r '.retry_after // 3' /tmp/discord-resp.json | cut -d. -f1)"
  echo "Discord tentativa $i falhou (HTTP $CODE)"; cat /tmp/discord-resp.json; echo; sleep 3
done
echo "::warning::Não consegui avisar no Discord (a versão foi publicada normalmente)."
exit 0
