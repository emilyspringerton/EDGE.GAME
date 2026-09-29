# EDGE.GAME -- Phase 1 build (see NORTHSTAR.md's own phased plan).
#
# client/traffic_router_gen.c and client/runtime/parena_runtime.{h,c} are vendored, generated
# copies (same convention `parena new`'s own scaffold uses) -- regenerate with:
#   cd ../PARENA && ./parena build stdlib/edge_game/traffic_router.prn \
#     -o ../EDGE.GAME/client/traffic_router_gen.c
# whenever traffic_router.prn itself changes.

CC ?= gcc

.PHONY: client run-relay test-e2e clean

client:
	$(CC) -std=c99 -Wall -Wextra -pedantic -Werror -I client/runtime \
		client/edge_client.c client/runtime/parena_runtime.c -o build/edge_client -lm

run-relay:
	node server/relay.js

# Real, local, no-hardware-needed end-to-end proof: starts the relay + a real client, sends real
# route commands through the actual HTTP->TCP->compiled-PARENA-decision path, checks the replies,
# then shuts both down. See NORTHSTAR.md's own "Phase 1" for what this proves and doesn't.
test-e2e: client
	EDGE_CLIENT_TOKEN=e2e-client-secret EDGE_OPERATOR_TOKEN=e2e-operator-secret \
	EDGE_CLIENT_PORT=18091 EDGE_OPERATOR_PORT=18092 \
	bash scripts/test_e2e.sh

clean:
	rm -rf build
