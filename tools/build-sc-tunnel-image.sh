#!/usr/bin/env bash
# build-sc-tunnel-image.sh [TAG] — Cloud Build the sc-tunnel image from EDGE.GAME's tracked files.
set -euo pipefail
SRC="$(cd "$(dirname "$0")/.." && pwd)"
TAG="${1:-$(git -C "$SRC" rev-parse --short HEAD)}"
PROJECT="${PROJECT:-project-d24a71e9-2daf-4b2d-917}"
CTX="$(mktemp -d)"; trap 'rm -rf "$CTX"' EXIT
git -C "$SRC" ls-files -z -- client/runtime vendor common tools/sc_tunnel.c | (cd "$SRC" && xargs -0 tar cf -) | tar xf - -C "$CTX"
mkdir -p "$CTX/tools"; cp "$SRC/tools/sc_tunnel.c" "$CTX/tools/"
cp "$SRC/tools/sc_tunnel.Dockerfile" "$CTX/Dockerfile"
gcloud builds submit "$CTX" --project "$PROJECT" --tag "us-central1-docker.pkg.dev/$PROJECT/emily/sc-tunnel:$TAG"
echo "sc-tunnel:$TAG"
