#!/usr/bin/env bash
# build-image.sh [TAG] — Cloud Build the JWT-enabled relay image. Requires `make relay` to have been
# run first (build/edge_relay). Fetches IDUNA's current public JWKS into the image (public data; a
# key rotation means rebuilding -- named limitation, see common/jwt_verify.h).
set -euo pipefail
SRC="$(cd "$(dirname "$0")/.." && pwd)"
TAG="${1:-$(git -C "$SRC" rev-parse --short HEAD)}"
PROJECT="${PROJECT:-project-d24a71e9-2daf-4b2d-917}"
[ -x "$SRC/build/edge_relay" ] || { echo "run 'make relay' first" >&2; exit 1; }
CTX="$(mktemp -d)"; trap 'rm -rf "$CTX"' EXIT
cp "$SRC/build/edge_relay" "$CTX/edge_relay"
curl -fsS "${IDUNA_JWKS_URL:-https://iam.okemily.com/.well-known/jwks.json}" -o "$CTX/jwks.json"
cp "$SRC/ops/docker/relay.Dockerfile" "$CTX/Dockerfile"
gcloud builds submit "$CTX" --project "$PROJECT" \
  --tag "us-central1-docker.pkg.dev/$PROJECT/emily/edge-relay:$TAG"
echo "edge-relay:$TAG"
