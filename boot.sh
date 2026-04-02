#!/bin/sh
# nowbox boot — one-shot agent sandbox
# Usage: curl -sSf https://nowbox.dev/boot | sh -s -- [agent]
#
# Agents: claude, codex, sh (default: sh)
# Requires: Apple Containers (macOS) or Docker
set -e

AGENT="${1:-sh}"
PORT="${NOWBOX_PORT:-8080}"
NAME="nowbox-$(date +%s)"
ARCH="$(uname -m)"
BINARY_URL="https://github.com/user/nowbox/releases/latest/download/nowbox-server-linux-${ARCH}"

# ── Detect container runtime ──
if command -v container >/dev/null 2>&1; then
  RT="container"
elif command -v docker >/dev/null 2>&1; then
  RT="docker"
else
  echo "error: no container runtime found (need Apple Containers or Docker)"
  exit 1
fi

echo "nowbox: using $RT"

# ── Agent bootstrap scripts ──
case "$AGENT" in
  claude)
    BOOTSTRAP='apk add --no-cache nodejs npm && npm install -g @anthropic-ai/claude-code'
    AGENT_CMD='claude'
    ;;
  codex)
    BOOTSTRAP='apk add --no-cache nodejs npm && npm install -g @openai/codex'
    AGENT_CMD='codex'
    ;;
  sh)
    BOOTSTRAP=''
    AGENT_CMD='/bin/sh'
    ;;
  *)
    echo "error: unknown agent '$AGENT' (try: claude, codex, sh)"
    exit 1
    ;;
esac

# ── Create temp Containerfile ──
TMPDIR=$(mktemp -d)
trap "rm -rf $TMPDIR" EXIT

# Download or use local binary
if [ -f "./server-c/nowbox-server" ] && file "./server-c/nowbox-server" | grep -q "ELF"; then
  cp ./server-c/nowbox-server "$TMPDIR/nowbox-server"
  echo "nowbox: using local binary"
else
  echo "nowbox: downloading binary..."
  curl -sSfL "$BINARY_URL" -o "$TMPDIR/nowbox-server" || {
    echo "error: failed to download binary"
    echo "hint: build locally with 'cd server-c && make'"
    exit 1
  }
fi

chmod +x "$TMPDIR/nowbox-server"

cat > "$TMPDIR/Containerfile" <<'EOF'
FROM alpine:latest
COPY nowbox-server /nowbox-server
RUN chmod +x /nowbox-server
EXPOSE 8080
EOF

if [ -n "$BOOTSTRAP" ]; then
  cat >> "$TMPDIR/Containerfile" <<EOF
ENV NOWBOX_BOOTSTRAP="$BOOTSTRAP"
ENV NOWBOX_AGENT="$AGENT_CMD"
CMD ["/nowbox-server"]
EOF
else
  cat >> "$TMPDIR/Containerfile" <<EOF
ENV NOWBOX_AGENT="$AGENT_CMD"
CMD ["/nowbox-server"]
EOF
fi

# ── Build & run ──
echo "nowbox: building image..."
$RT build -t "$NAME:latest" "$TMPDIR" >/dev/null 2>&1

echo "nowbox: starting $AGENT..."
$RT run --name "$NAME" -p "$PORT:8080" -d "$NAME:latest" >/dev/null 2>&1

# ── Wait for server to start ──
sleep 1
for i in 1 2 3 4 5; do
  TOKEN=$($RT logs "$NAME" 2>&1 | grep "token:" | awk '{print $2}')
  if [ -n "$TOKEN" ]; then break; fi
  sleep 1
done

if [ -z "$TOKEN" ]; then
  echo "error: server failed to start"
  $RT logs "$NAME" 2>&1
  exit 1
fi

URL="http://localhost:${PORT}/#t=${TOKEN}"

echo ""
echo "  nowbox ready"
echo "  agent: $AGENT"
echo "  url:   $URL"
echo ""
echo "  stop:  $RT stop $NAME && $RT rm $NAME"
echo ""

# Try to open in browser
if command -v open >/dev/null 2>&1; then
  open "$URL"
fi
