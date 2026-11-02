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

# ---- commits + lines: baseline + increment.
# The baseline (data/baseline.txt) was computed once with the owner's own login by
# fetch-local.sh: full contribution history, paint excluded. Actions can't recompute it,
# because the workflow token can't see all history (org repos hidden; ~1k commits short).
# So Actions adds only what it CAN see from the day after the baseline onward:
#   commits += public commits authored by the user (commit search, default branches only)
#   lines   += additions in those commits, skipping notebooks, lockfiles, minified files
#              and SVGs (so regenerated card assets never count as written code)
# Known gaps, accepted: commits to private repos and non-default branches after the
# baseline aren't counted. To re-anchor exactly, run scripts/fetch-local.sh on the Mac
# (rewrites the baseline) and push.
baseline_date="$(awk '$1 == "baseline_date" {print $2}' "$ROOT/data/baseline.txt")"
base_commits="$(awk '$1 == "commits" {print $2}' "$ROOT/data/baseline.txt")"
base_lines="$(awk '$1 == "lines" {print $2}' "$ROOT/data/baseline.txt")"

new_commits=0
new_lines=0
while read -r repo sha; do
  [ -n "$sha" ] || continue
  added="$(gh api "repos/${repo}/commits/${sha}" --jq \
    '[.files[]? | select(.filename | test("\\.ipynb$|lock\\.json$|\\.lock$|\\.min\\.|\\.svg$") | not) | .additions] | add // 0')"
  new_commits=$((new_commits + 1))
  new_lines=$((new_lines + added))
done < <(gh api -X GET search/commits --paginate \
  -f q="author:${USER_LOGIN} committer-date:>${baseline_date} -repo:${USER_LOGIN}/${EXCLUDED_REPO}" \
  -f per_page=100 --jq '.items[] | "\(.repository.full_name) \(.sha)"')

echo "commits $((base_commits + new_commits))"
echo "lines $((base_lines + new_lines))"

# ---- ai: refreshed on the Mac (data/ai.json); local usage logs don't exist on runners.
jq -r '.[] | "model \(.maker) \(.family) \(.tokens // -1)"' "$ROOT/data/ai.json"
