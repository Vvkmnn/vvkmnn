#!/usr/bin/env bash
# Fetches every live number on the profile card and prints data.txt for card.c.
# Needs: gh (authenticated; in Actions GH_TOKEN=github.token is enough — all data is public), curl, jq.
# Format: one record per line, space-separated, first field = record type.
#   installs N | daily N | stars N | forks N | commits N | lines N | langs_total N
#   pkg NAME TOTAL V1 V2 ...   (daily installs on a shared axis starting at the first publish)
#   lang NAME REPOS
#   model MAKER FAMILY TOKENS  (TOKENS = -1 when no usage was logged)
set -euo pipefail

readonly USER_LOGIN="Vvkmnn"
readonly NPM_AUTHOR="vvkmnn"
readonly EXCLUDED_REPO="paint" # backdated gitfiti art; excluded from every number
# Hand-written additions, notebooks/vendored excluded (GitHub stats/contributors sweep,
# 2026-11-02). Too slow/flaky to recompute daily: stats endpoints return 202 for minutes.
readonly LINES=433435
readonly HERE="$(cd "$(dirname "$0")" && pwd)"
readonly ROOT="$(dirname "$HERE")"

today="$(date -u +%F)"

# ---- npm: every package by the author; the original `claude-historian` merges into historian.
packages="$(curl -fsS "https://registry.npmjs.org/-/v1/search?text=maintainer:${NPM_AUTHOR}&size=100" \
  | jq -r '.objects[].package.name')"
packages="$(printf '%s\nclaude-historian\n' "$packages" | sort -u)"

series=""
for pkg in $packages; do
  name="${pkg#claude-}"
  name="${name%-mcp}"
  # Range API caps at 18 months per call; packages are newer than 2025-06.
  series+="$(curl -fsS "https://api.npmjs.org/downloads/range/2025-06-01:${today}/${pkg}" \
    | jq -c --arg name "$name" '{name: $name, days: [.downloads[] | {day, n: .downloads}]}')"$'\n'
done

jq -rs '
  # merge packages sharing a name (historian + historian-mcp), then align on one day axis
  group_by(.name)
  | map({name: .[0].name, days: ([.[].days[]] | group_by(.day) | map({day: .[0].day, n: (map(.n) | add)}))})
  | (map(.days[]) | map(select(.n > 0)) | map(.day) | min) as $first
  | map(.days |= map(select(.day >= $first)) | .days |= sort_by(.day))
  | (map(.days | map(.n) | add) | add) as $total
  | (.[0].days | length) as $span
  | "installs \($total)",
    "daily \(($total / $span) | round)",
    (sort_by(-(.days | map(.n) | add))[] | "pkg \(.name) \(.days | map(.n) | add) \(.days | map(.n) | join(" "))")
' <<< "$series"

# ---- GitHub: public non-fork repos (languages, stars, forks), paint excluded.
gh api graphql -f query="{ user(login:\"${USER_LOGIN}\"){ repositories(ownerAffiliations:OWNER,isFork:false,privacy:PUBLIC,first:100){
  nodes{ name stargazerCount forkCount languages(first:30){ nodes{ name } } } } } }" \
  | jq -r --arg ex "$EXCLUDED_REPO" '
    [.data.user.repositories.nodes[] | select(.name != $ex)] as $repos
    | ["HTML","CSS","SCSS","Less","TeX","Makefile","Dockerfile","CMake","Just","Starlark","XSLT","Liquid","Metal"] as $markup
    | "stars \($repos | map(.stargazerCount) | add)",
      "forks \($repos | map(.forkCount) | add)",
      ([$repos[] | [.languages.nodes[].name | if . == "Jupyter Notebook" then "Python" else . end] | unique[]]
        | map(select(. as $l | $markup | index($l) | not))
        | group_by(.) | map({lang: .[0], n: length}) | sort_by(-.n, .lang)) as $langs
      | "langs_total \($langs | length)",
        ($langs[] | "lang \(.lang | gsub(" "; "_")) \(.n)")
  '

# ---- lifetime commits: per-year contributions, paint excluded (account created 2012).
commits=0
for year in $(seq 2012 "$(date -u +%Y)"); do
  n="$(gh api graphql -f query="{ user(login:\"${USER_LOGIN}\"){ contributionsCollection(from:\"${year}-01-01T00:00:00Z\", to:\"${year}-12-31T23:59:59Z\"){
    commitContributionsByRepository(maxRepositories:100){ repository{ name owner{ login } } contributions{ totalCount } } } } }" \
    | jq --arg ex "$EXCLUDED_REPO" --arg me "$USER_LOGIN" \
      '[.data.user.contributionsCollection.commitContributionsByRepository[]
        | select((.repository.name == $ex and .repository.owner.login == $me) | not)
        | .contributions.totalCount] | add // 0')"
  commits=$((commits + n))
done
echo "commits ${commits}"
echo "lines ${LINES}"

# ---- ai: refreshed on the Mac (data/ai.json); local usage logs don't exist on runners.
jq -r '.[] | "model \(.maker) \(.family) \(.tokens // -1)"' "$ROOT/data/ai.json"
