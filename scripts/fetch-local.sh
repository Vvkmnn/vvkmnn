#!/usr/bin/env bash
# OPTIONAL re-anchor of the commits/lines baseline. Not part of the daily automation.
#
# Run on the Mac with `gh` logged in as Vvkmnn, then commit + push data/baseline.txt.
# The daily Action (fetch.sh) then counts increments from the new baseline_date.
#
# Why this exists: the workflow's GITHUB_TOKEN can't see all contribution history
# (org repos are hidden from it), so lifetime commits come out ~1k too low in Actions.
# Your own login sees everything, so the exact lifetime number is computed here.
#
# LINES is NOT recomputed here: it came from a one-off GitHub stats/contributors sweep on
# 2026-11-02 (hand-written additions, notebook-heavy and vendored repos excluded). That
# endpoint returns 202 "computing" for minutes per repo, so it isn't scripted. Re-anchoring
# keeps lines = previous baseline + whatever fetch.sh last counted since then.
set -euo pipefail

readonly USER_LOGIN="Vvkmnn"
readonly EXCLUDED_REPO="paint" # backdated gitfiti art; excluded from every number
readonly ROOT="$(cd "$(dirname "$0")/.." && pwd)"
readonly BASELINE="$ROOT/data/baseline.txt"

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

# Carry lines forward from the last rendered total (baseline + counted increments).
lines="$(awk '$1 == "lines" {print $2}' "$ROOT/data/data.txt")"

printf 'baseline_date %s\ncommits %s\nlines %s\n' "$(date -u +%F)" "$commits" "$lines" > "$BASELINE"
cat "$BASELINE"
