# EDGE.GAME build (see NORTHSTAR.md's own phased plan).
#
# client/traffic_router_gen.c and client/runtime/parena_runtime.{h,c} are vendored, generated
# copies (same convention `parena new`'s own scaffold uses) -- regenerate with:
#   cd ../PARENA && ./parena build stdlib/edge_game/traffic_router.prn \
#     -o ../EDGE.GAME/client/traffic_router_gen.c
# whenever traffic_router.prn itself changes.
#
# server/reflux_gen.ll and server/tcp_llvm_gen.ll are ALSO vendored, generated copies -- same
# convention, different target. Rewritten 2026-09-29 (founder real-time: "write it in PARENA in
# what world are we using node for any part of this stack?" -> "parena really needs to just emit
# the fucking llvm code for the server... i think we eat that tech debt" -- see
# server/relay_main.c's own header comment for the full real architecture): the relay is no longer
# Node.js. It's a real native binary (`build/edge_relay`) linking genuine PARENA-compiled object
# code -- stdlib/reflux/reflux.prn (the event pub/sub log) and stdlib/net/tcp_llvm.prn (raw
# listen/accept/close) -- emitted through PARENA's new src/emit_llvm.c `#target {:llvm ...}` FFI
# hatch, lowered to x86_64 machine code by `llc` (PARENA_ROOT/LLVM_TOOLCHAIN_ROOT below), and linked
# against a thin hand-written C host (server/relay_main.c) for the select()/NDJSON plumbing that
# target's v0 genuinely can't express (no struct/array/String support -- see tcp_llvm.prn's own
# header comment). Regenerate the two .ll files with:
#   cd ../PARENA && ./parena build stdlib/reflux/reflux.prn -o ../EDGE.GAME/server/reflux_gen.ll
#   cd ../PARENA && ./parena build stdlib/net/tcp_llvm.prn -o ../EDGE.GAME/server/tcp_llvm_gen.ll
# whenever either .prn source changes (the `relay` target below does this automatically).

CC ?= gcc
PARENA_ROOT ?= ../PARENA
LLVM_TOOLCHAIN_ROOT ?= $(HOME)/.local/opt/llvm-toolchain
LLC := $(LLVM_TOOLCHAIN_ROOT)/usr/lib/llvm-18/bin/llc
LLVM_LIB_PATH := $(LLVM_TOOLCHAIN_ROOT)/usr/lib/x86_64-linux-gnu:$(LLVM_TOOLCHAIN_ROOT)/usr/lib/llvm-18/lib

CC_WIN ?= x86_64-w64-mingw32-gcc

.PHONY: test-usb-probe client client-windows relay run-relay test-e2e clean

build:
	mkdir -p build

client: build
	$(CC) -std=c99 -Wall -Wextra -pedantic -Werror -I client/runtime \
		client/edge_client.c client/usb_probe.c client/runtime/parena_runtime.c -o build/edge_client -lm

# client-windows -- real mingw cross-compile of the same edge_client.c (CI auto-release, S584
# cont. 3, founder real-time: "get CICD auto releases set up for the game client"). edge_client.c
# was already written portable against this exact target (#ifdef _WIN32 for winsock2, see its own
# header comment) -- this target is the first thing that actually EXERCISES that, not just trusts
# it compiles. -DPARENA_NO_GRAPHICS skips the shared runtime header's unconditional
# <SDL2/SDL.h> include: edge_client.c is still Phase 1's headless NDJSON test client (no SDL2 UI
# yet, see its own header comment -- that's Phase 2+), and this sandbox has no SDL2-for-mingw
# cross-build tree, so pulling in a library nothing calls would need one for no real reason.
# Native `client` above stays unconditional/unguarded, matching every other consumer of this
# shared runtime header in this monorepo (PARENA/docs: "unconditionally available, harmless if
# unused") -- this is a narrow, CI-only carve-out, not a repo-wide convention change.
client-windows: build
	$(CC_WIN) -std=c99 -Wall -Wextra -pedantic -Werror -DPARENA_NO_GRAPHICS -I client/runtime \
		client/edge_client.c client/usb_probe.c client/runtime/parena_runtime.c -o build/edge_client.exe -lws2_32 -ladvapi32 -lm

# test-usb-probe -- classification, JSON, and POSIX enumeration against a fake sysfs tree.
test-usb-probe: build
	$(CC) -std=c99 -Wall -Wextra -pedantic -Werror -D_POSIX_C_SOURCE=200809L -Iclient \
		tests/test_usb_probe.c client/usb_probe.c -o build/test_usb_probe
	./build/test_usb_probe

# relay -- the real, native PARENA+C server binary. Regenerates both .ll files from their real
# .prn sources every time (cheap, and keeps the checked-in vendored copies honest), lowers each to
# a real x86_64 object file via llc, compiles the hand-written C pieces, and links everything into
# one binary. Needs a sibling ../PARENA checkout with `parena` already built (`make build` there)
# and the real, no-sudo-acquired LLVM toolchain PARENA/docs/LLVM_BACKEND_NORTHSTAR.md documents
# acquiring (`apt-get download llvm-18 clang-18 ...` + `dpkg -x`).
relay: build
	cd $(PARENA_ROOT) && ./parena build stdlib/reflux/reflux.prn -o ../EDGE.GAME/server/reflux_gen.ll
	cd $(PARENA_ROOT) && ./parena build stdlib/net/tcp_llvm.prn -o ../EDGE.GAME/server/tcp_llvm_gen.ll
	LD_LIBRARY_PATH=$(LLVM_LIB_PATH) $(LLC) -mtriple=x86_64-pc-linux-gnu -filetype=obj \
		server/reflux_gen.ll -o server/reflux_gen.o
	LD_LIBRARY_PATH=$(LLVM_LIB_PATH) $(LLC) -mtriple=x86_64-pc-linux-gnu -filetype=obj \
		server/tcp_llvm_gen.ll -o server/tcp_llvm_gen.o
	$(CC) -std=c99 -Wall -Wextra -pedantic -Werror -c server/reflux_runtime.c -o server/reflux_runtime.o
	$(CC) -std=c99 -Wall -Wextra -pedantic -Werror -c server/tcp_llvm_glue.c -o server/tcp_llvm_glue.o
	$(CC) -std=c99 -Wall -Wextra -pedantic -Werror -c server/relay_main.c -o server/relay_main.o
	$(CC) -o build/edge_relay server/relay_main.o server/reflux_runtime.o server/tcp_llvm_glue.o \
		server/reflux_gen.o server/tcp_llvm_gen.o

run-relay: relay
	./build/edge_relay

# Real, local, no-hardware-needed end-to-end proof: starts the real relay binary + a real client,
# sends real route commands through the actual TCP+NDJSON -> compiled-PARENA-decision path, checks
# the replies, exercises the REFLUX events channel (buffered + live), then shuts both down. See
# NORTHSTAR.md's own phased plan for what this proves and doesn't.
test-e2e: client relay
	EDGE_CLIENT_TOKEN=e2e-client-secret EDGE_OPERATOR_TOKEN=e2e-operator-secret \
	EDGE_CLIENT_PORT=18091 EDGE_OPERATOR_PORT=18092 \
	bash scripts/test_e2e.sh

clean:
	rm -rf build
	rm -f server/*.o server/reflux_gen.ll server/tcp_llvm_gen.ll
