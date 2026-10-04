# sc-tunnel image: PARENA net/secure_channel TCP tunnel (client sidecar / server). Build context =
# EDGE.GAME tracked files; see tools/build-sc-tunnel-image.sh.
FROM debian:12-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends gcc libc6-dev libsdl2-dev libsdl2-ttf-dev libsdl2-image-dev libsdl2-mixer-dev ca-certificates && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN mkdir -p /v && cd /v && gcc -std=c99 -O2 -D_GNU_SOURCE -w -I /src/vendor/mlkem -c /src/vendor/mlkem/*.c /src/vendor/aead/monocypher.c \
 && gcc -std=gnu99 -O2 -Wall -Wextra -Werror -I /src/client/runtime -I /src/vendor -I /src/vendor/mlkem -DPARENA_WITH_MLKEM -DPARENA_WITH_AEAD \
      /src/tools/sc_tunnel.c /src/common/sec_transport.c /src/client/runtime/parena_runtime.c /v/*.o -o /sc_tunnel -lm
FROM debian:12-slim
COPY --from=build /sc_tunnel /sc_tunnel
USER 65532:65532
ENTRYPOINT ["/sc_tunnel"]
