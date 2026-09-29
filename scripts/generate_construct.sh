#!/usr/bin/env bash
set -euo pipefail

# Generate a deterministic EDGE.GAME source construct from tracked files (monorepo Principle 21).
# Same git tree produces byte-for-byte identical output regardless of runner or timestamp.
# Pattern ported from DEADWEIGHT_2/scripts/generate_construct.sh (same real precedent).

export LC_ALL=C

OUT="${1:-EDGE_GAME_CONSTRUCT.txt}"
TREE_SHA="$(git rev-parse --verify HEAD^{tree} 2>/dev/null || printf 'unknown')"
TMP_FILES="$(mktemp)"
trap 'rm -f "$TMP_FILES"' EXIT

# Capture only tracked files; exclude generated outputs, dependencies, and this script's own output.
git ls-files -z \
  ':(exclude)EDGE_GAME_CONSTRUCT*.txt' \
  ':(exclude)MANIFEST*.txt' \
  ':(exclude).git/**' \
  ':(exclude)vendor/**' \
  ':(exclude)node_modules/**' \
  ':(exclude)build/**' \
  ':(exclude)dist/**' \
  | sort -z > "$TMP_FILES"

{
  printf 'EDGE.GAME CONSTRUCT\n'
  printf 'schema_version: 1\n'
  printf 'tree_sha: %s\n' "$TREE_SHA"
  printf 'source: git ls-files\n'
  printf '\n'
} > "$OUT"

COUNT=0
while IFS= read -r -d '' file; do
  [ -f "$file" ] || continue
  SHA="$(sha256sum "$file" | awk '{print $1}')"
  SIZE="$(wc -c < "$file")"
  MODE="$(git ls-files -s -- "$file" | awk '{print $1}')"

  {
    printf -- '--- FILE START: %s ---\n' "$file"
    printf 'sha256: %s\n' "$SHA"
    printf 'size_bytes: %s\n' "$SIZE"
    printf 'git_mode: %s\n' "$MODE"
    printf 'encoding: text\n'
    printf -- '--- CONTENT START ---\n'
    cat "$file"
    printf '\n'
    printf -- '--- CONTENT END ---\n'
    printf -- '--- FILE END: %s ---\n' "$file"
    printf '\n'
  } >> "$OUT"
  COUNT=$((COUNT + 1))
done < "$TMP_FILES"

printf "Generated %s with %d files (tree_sha: %s)\n" "$OUT" "$COUNT" "$TREE_SHA"
