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
# CI SA constraints (EMILY/gitops/CI_SETUP.md, found live 2026-10-05; same recipe as WOTAN's
# build-image.sh): pin the staging dir so gcloud does not list buckets project-wide (403), and
# --async + poll status instead of streaming logs (needs a Viewer role the CI SA deliberately lacks).
BUILD_ID=$(gcloud builds submit "$CTX" --project "$PROJECT" \
  --tag "us-central1-docker.pkg.dev/$PROJECT/emily/edge-relay:$TAG" \
  --gcs-source-staging-dir="gs://${PROJECT}_cloudbuild/source" \
  --async --format="value(id)")
echo "submitted build $BUILD_ID, polling for completion..."
while true; do
  STATUS=$(gcloud builds describe "$BUILD_ID" --project "$PROJECT" --format="value(status)")
  case "$STATUS" in
    SUCCESS) echo "edge-relay:$TAG"; exit 0 ;;
    FAILURE|INTERNAL_ERROR|TIMEOUT|CANCELLED|EXPIRED) echo "build $BUILD_ID: $STATUS" >&2; exit 1 ;;
    *) sleep 5 ;;
  esac
done
