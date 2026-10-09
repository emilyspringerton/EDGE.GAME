# EDGE.GAME relay image (JWT-enabled build). Binary-staging STOPGAP, same convention DEADWEIGHT's
# image uses: scripts/build-image.sh stages build/edge_relay (built via `make relay`) + IDUNA's
# current public JWKS into the build context. A hermetic from-source build needs the PARENA
# compiler + LLVM toolchain inside Cloud Build -- tracked as a follow-up. Ubuntu 24.04 base so the
# glibc/OpenSSL match the sandbox the binary is built on.
FROM ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends libssl3t64 ca-certificates && rm -rf /var/lib/apt/lists/*
COPY edge_relay /app/edge_relay
COPY jwks.json /app/jwks.json
RUN chmod +x /app/edge_relay && mkdir -p /app/var && chown -R 65532:65532 /app/var
ENV EDGE_IDUNA_JWKS_FILE=/app/jwks.json EDGE_KEY_FILE=/app/var/edge_relay.key EDGE_CLIENT_PORT=8091 EDGE_OPERATOR_PORT=8092
WORKDIR /app/var
USER 65532
ENTRYPOINT ["/app/edge_relay"]
