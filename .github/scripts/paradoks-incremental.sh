#!/usr/bin/env bash
# Incremental C++ builds on CI without a compiler cache.
#
# MSBuild decides what to rebuild by comparing file times. A fresh checkout gives every source
# the current time, so a restored build directory would be rebuilt completely. This script
# keeps a manifest (content hash of every tracked file, the repository and all submodules)
# next to the build directory. After the build directory is restored it:
#   1. gives all build outputs the current time (they are considered up to date), then
#   2. touches only the sources whose content differs from the manifest (they get rebuilt).
#
# Usage:
#   paradoks-incremental.sh manifest <src dir> <manifest file>
#   paradoks-incremental.sh prepare  <src dir> <out dir>
#   paradoks-incremental.sh save     <src dir> <out dir>
#   paradoks-incremental.sh submanifest            (internal, runs inside `git submodule foreach`)
set -euo pipefail

SCRIPT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/$(basename "${BASH_SOURCE[0]}")"
MANIFEST_NAME=".paradoks_src_manifest"

submanifest() {
  # $displaypath is exported by `git submodule foreach`.
  git ls-files -s | awk -F'\t' -v p="${displaypath}/" '{ split($1, a, " "); print a[2] "\t" p $2 }'
  git diff --name-only | awk -v p="${displaypath}/" '{ print "MODIFIED\t" p $0 }'
}

make_manifest() {
  local src="$1" out="$2" tmp
  tmp="$(mktemp)"
  (
    cd "$src"
    git ls-files -s | awk -F'\t' '{ split($1, a, " "); print a[2] "\t" $2 }'
    git diff --name-only | awk '{ print "MODIFIED\t" $0 }'
    git submodule foreach --quiet --recursive "bash '$SCRIPT' submanifest"
  ) | LC_ALL=C sort -u > "$tmp"
  mv "$tmp" "$out"
}

case "${1:-}" in
  submanifest)
    submanifest
    ;;
  manifest)
    make_manifest "$2" "$3"
    ;;
  save)
    make_manifest "$2" "$3/$MANIFEST_NAME"
    echo "Manifest saved: $(wc -l < "$3/$MANIFEST_NAME") entries."
    ;;
  prepare)
    src="$2"; out="$3"
    old="$out/$MANIFEST_NAME"
    if [ ! -f "$old" ]; then
      echo "No manifest in the restored build directory: full build."
      exit 0
    fi
    new="$(mktemp)"
    make_manifest "$src" "$new"
    changed="$(mktemp)"
    LC_ALL=C comm -13 "$old" "$new" | cut -f2- | LC_ALL=C sort -u > "$changed"
    echo "Sources changed since the cached build: $(wc -l < "$changed")"
    # 1. Everything that was built is "now".
    find "$out" -type f ! -name "$MANIFEST_NAME" -exec touch {} +
    sleep 3
    # 2. Changed sources are newer than the outputs, so they (and their dependents) rebuild.
    while IFS= read -r path; do
      [ -n "$path" ] && [ -e "$src/$path" ] && touch "$src/$path"
    done < "$changed"
    echo "Incremental preparation finished."
    ;;
  *)
    echo "Usage: $0 {manifest|prepare|save} ..." >&2
    exit 2
    ;;
esac
