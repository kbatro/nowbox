#!/bin/bash
# Build script for nowbox-server
# Workaround for zig build system broken on macOS 26
set -e

CACHE="$HOME/.cache/zig/p"
HTTPZ="$CACHE/httpz-0.0.0-PNVzrMpOBwCS42DpyNnY8WTlwcuzannBeDLuqkUV8H7-/src"
METRICS="$CACHE/metrics-0.0.0-W7G4eP2_AQAdJGKMonHeZFaY4oU4ZXPFFTqFCFXItX3O/src"
WEBSOCKET="$CACHE/websocket-0.1.0-ZPISdZJxAwAt6Ys_JpoHQQV3NpWCof_N9Jg-Ul2g7OoV/src"

TARGET="${1:-aarch64-macos.15.0}"
OUT="${2:-nowbox-server}"

cd "$(dirname "$0")"

echo "building nowbox-server for $TARGET..."

# Module dependency graph:
#   main -> httpz, tokens, session, pty
#   httpz -> metrics, websocket, build
#   session -> httpz, tokens
#   tokens -> (none)
#   pty -> (none)

zig build-exe \
  -target "$TARGET" \
  -lc \
  --dep httpz --dep tokens --dep session --dep pty \
  -Mmain=src/main.zig \
  -Mtokens=src/tokens.zig \
  -Mpty=src/pty.zig \
  --dep metrics --dep websocket --dep build \
  -Mhttpz="$HTTPZ/httpz.zig" \
  -Mmetrics="$METRICS/metrics.zig" \
  --dep build \
  -Mwebsocket="$WEBSOCKET/websocket.zig" \
  -Mbuild=src/build_options.zig \
  --dep httpz --dep tokens \
  -Msession=src/session.zig \
  --name "$OUT" \
  -O ReleaseSafe

echo "built: ./$OUT ($(du -h "$OUT" | cut -f1))"
