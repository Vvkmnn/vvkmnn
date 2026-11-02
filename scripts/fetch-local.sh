#!/usr/bin/env bash
# Owner-only numbers, run on the Mac (needs `gh` logged in as Vvkmnn). Writes data/local.txt.
# Why not in Actions: the workflow's GITHUB_TOKEN can't see all contribution history
# (org repos are hidden from it), so lifetime commits come out ~1k too low there.
set -euo pipefail

readonly USER_LOGIN="Vvkmnn"
readonly EXCLUDED_REPO="paint" # backdated gitfiti art; excluded from every number
# Hand-written additions, notebooks/vendored excluded (stats/contributors sweep, 2026-11-02).
readonly LINES=433435
readonly ROOT="$(cd "$(dirname "$0")/.." && pwd)"

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

printf 'commits %s\nlines %s\n' "$commits" "$LINES" > "$ROOT/data/local.txt"
cat "$ROOT/data/local.txt"
