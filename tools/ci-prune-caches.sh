#!/usr/bin/env bash
# ANTS-5532 — keep one GitHub Actions cache entry per key prefix on this ref.
#
# Usage: tools/ci-prune-caches.sh <key-prefix> <kept-key>
#
# Each CI job saves its cache under a per-SHA key, and a key cannot be
# overwritten, so every push adds an entry. Only the newest is ever restored
# (restore-keys picks the latest with the prefix). The rest filled the
# repository past GitHub's 10 GB limit and got the nightly ASan cache evicted.
#
# Run after the job's save step. It deletes nothing unless <kept-key> is
# listed, so a failed save never leaves the job with no cache. A failure
# warns and exits 0: a cache problem must not turn a clean run red.
# Needs GH_TOKEN with actions: write, GITHUB_REPOSITORY and GITHUB_REF.
# Contract: tests/features/ci_cache_prune/spec.md.

set -uo pipefail

prefix=${1:?usage: ci-prune-caches.sh <key-prefix> <kept-key>}
keep=${2:?usage: ci-prune-caches.sh <key-prefix> <kept-key>}
repo=${GITHUB_REPOSITORY:?GITHUB_REPOSITORY unset}
ref=${GITHUB_REF:?GITHUB_REF unset}

warn() {
    echo "::warning::ci-prune-caches: $*"
    exit 0
}

# The key filter is a prefix match. One line per entry: "<id> <key>".
listing=$(gh api --paginate -X GET "repos/$repo/actions/caches" \
    -f key="$prefix" -f ref="$ref" \
    --jq '.actions_caches[] | "\(.id) \(.key)"') \
    || warn "could not list caches for $prefix on $ref"

if ! cut -d' ' -f2- <<<"$listing" | grep -qxF -- "$keep"; then
    echo "ci-prune-caches: $keep is not saved; deleting nothing"
    exit 0
fi

deleted=0
while read -r id key; do
    [[ -z $id || $key == "$keep" ]] && continue
    gh api -X DELETE "repos/$repo/actions/caches/$id" >/dev/null \
        || warn "could not delete cache $id ($key)"
    echo "deleted cache $id ($key)"
    deleted=$((deleted + 1))
done <<<"$listing"

echo "ci-prune-caches: kept $keep, deleted $deleted"
