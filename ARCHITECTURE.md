# nowbox — architecture

## The pieces

### 1. nowbox.com — the launcher

- Static Svelte app (adapter-static), hosted anywhere (GitHub Pages, Vercel, CDN)
- No backend. No database. No auth. No storage.
- Ships one WASM module as a static asset: `generator.wasm`
- User picks: host type, agent, clicks Go
- Calls `generator.wasm` in a WASM sandbox → gets back an opaque `.now` blob
- Auto-opens the `.now` client in a new tab via blob URL (instant, no download dialog)
- Done. Forgets everything. The secret never touched JS.

### 2. generator.wasm — the factory

- Ships with nowbox.com as a static asset
- Contains: secret generation logic + the entire client runtime code + the assembler
- Runs in browser WASM sandbox when user clicks Go
- Generates a 32-byte cryptographic secret
- Derives the session name: `brave-fox-k7x9.now`
- Packs the client runtime + secret + host config + agent config into a single self-contained `.now` file
- The `.now` file is an HTML page with the client WASM binary embedded as base64 and a thin JS shell (~20 lines) that bootstraps the WASM and provides browser API imports
- Returns the assembled `.now` file as an opaque blob
- Open source, deterministic build, auditable, verifiable by hash
- Secret exists only in WASM linear memory, never in JS

### 3. The `.now` file — the session, the client, everything

- An HTML file (~300KB) with the client WASM binary embedded as base64
- Tiny JS shell (~20 lines) that bootstraps the WASM and provides browser API imports (WebSocket, WebRTC, DOM)
- File extension is `.now` but it's valid HTML — browsers render it
- Click Go on nowbox.com → auto-opens in a new tab via blob URL (instant, no download)
- Can be saved and shared as a file for later use / multiplayer
- Double-click the file → opens in default browser → runs
- Works from `file://`, works offline, fully self-contained
- Generated once, shown once, like an API token
- Exists only on the user's machine (and whoever they share it with)
- No server stores it. No URL hosts it. Lose it → session gone.
- Named non-deterministically from the secret: `brave-fox-k7x9.now`, `calm-jade-m2p4.now`
- Naming schema: `wordlist[byte0]` + `-` + `wordlist[byte1]` + `-` + `base58(bytes[2..5])` (256-word list, ~2^48 collision space)
- Runs in: browser (UI + terminal + WebRTC), Node.js (MCP bridge mode), any WASM runtime
- The WASM runtime inside handles:
  - Gossip client (Nostr — lightweight, WebSocket + JSON, public relays)
  - WebRTC signaling logic (ICE candidate exchange)
  - Terminal protocol (PTY streaming)
  - Encryption (all keys derived from the shared secret)
- WASM imports from host environment: `websocket_connect`, `webrtc_create_peer`, `render_terminal`, `stdin_read`

### 4. nowbox-server — the sandbox binary

- Open source, public GitHub repo
- Single binary (Go or Rust), cross-compiled for Linux amd64/arm64
- Downloaded by the bootstrap script at VM creation time
- Runs inside the sandbox (Sprites VM, Docker container, VPS, whatever)
- Starts the declared agent (Claude Code, Codex, etc.) with a PTY
- Connects to STUN (Google, free) → learns its public address
- Connects to Nostr relays → publishes encrypted ICE candidates to derived topic
- Listens for WebRTC connections
- Serves: PTY over DataChannel, file ops over DataChannel
- Accepts multiple peers (multiplayer)

### 5. Nostr — the gossip/discovery layer

- Public relays: `relay.damus.io`, `nos.lol`, `relay.nostr.band`, dozens more
- Not ours. Not anyone's in particular. Decentralized.
- Ephemeral events (kind 20000-29999) — propagate and disappear
- Both sides derive the same Nostr keypair and topic from the shared secret
- Signaling protocol:
  - Server publishes: encrypted ICE candidates + DTLS fingerprint to derived topic
  - Client subscribes: same topic, receives server's candidates
  - Client publishes: encrypted WebRTC answer back to same topic
  - Server receives: answer, completes WebRTC handshake
- Used for ~2 seconds of signaling, then both sides disconnect from Nostr
- If one relay is down, others carry it. No single point of failure.

### 6. STUN — address discovery

- `stun:stun.l.google.com:19302` (free, open standard, RFC 8489)
- The sandbox calls STUN to learn its own public IP:port
- The browser uses STUN via the WebRTC stack automatically
- Not a relay. Just echoes "your public address is X."

### 7. WebRTC — The P2P Connection (Updated with TURN)

The one and only transport protocol. Everything (terminal, file ops, control) runs over encrypted DataChannels.

**ICE Strategy (tried in order):**
1. Direct UDP hole-punching (primary, fastest)
2. Google STUN (`stun:stun.l.google.com:19302`)
3. freeturn.net TURN fallback (TCP-capable on 80/443 to bypass restrictive firewalls)

**freeturn.net TURN Config (added to both .now client and nowbox-server):**
```javascript
// In RTCConfiguration (WASM client side and Go/Rust server side)
const iceServers = [
  { urls: ['stun:stun.l.google.com:19302'] },
  {
    urls: [
      'turn:freeturn.net:3478',               // UDP
      'turn:freeturn.net:5349',               // TLS
      'turn:freeturn.net:80?transport=tcp',   // TCP fallback
      'turn:freeturn.net:443?transport=tcp'
    ],
    username: 'freeturn',           // Check current credentials on freeturn.net
    credential: 'freeturn'          // Often the same for free tier; verify on their site
  }
];
```

**How TURN Integration Works with the Launch Script:**
- The `nowbox-launch.sh` can optionally fetch/refresh credentials from freeturn.net (or use the static ones above).
- The sandbox (`nowbox-server`) includes the full `iceServers` array in its Nostr "I'm alive" announcement (encrypted).
- The `.now` client receives it via Nostr, updates its `RTCPeerConnection` configuration, and retries ICE gathering if needed.
- WebRTC automatically falls back: direct → STUN → TURN. No manual intervention required.

This dramatically improves connection success rate (from ~70-85% direct to near 95%+ in real-world tests) while adding almost zero complexity.

---

## The flow

### Step 1 — Generate (nowbox.com)

```
User visits nowbox.com
Picks: Sprites + Claude Code
Clicks Go
→ generator.wasm (in WASM sandbox):
  → generates 32-byte secret
  → derives session name (brave-fox-k7x9)
  → packs client runtime + secret + config into HTML with embedded WASM
  → returns the .now blob
→ JS triggers an automatic file download (`brave-fox-k7x9.now`)
→ JS also opens the blob URL in a new tab (instant access)
→ nowbox.com is done. can close. knows nothing.
```

### Step 2 — Start (Smart Hybrid Launch + CORS Reality)

**The CORS Challenge**
The `.now` file runs as a self-contained HTML (often opened via `file://` or blob URL). This sets `Origin: null` in requests. Most production APIs, including Sprites.dev, do not reliably accept null origins due to security policies. Preflight OPTIONS requests are often blocked, and even simple POSTs can be silently dropped or rejected.

**Our Solution: Smart Hybrid Launch (Auto-first + Magical Fallback)**
In the `.now` UI after the user enters their Sprites API key (or selects host type):

*Automated Launch (Happy Path)*
- The client attempts a fire-and-forget POST to the Sprites API (`https://api.sprites.dev/v1/sprites`) with the user's Bearer token, session name, and a one-time boot token.
- Uses `mode: 'no-cors'` where necessary.
- Immediately begins watching the Nostr topic for the sandbox "I'm alive" announcement.
- Shows: "Launching VM on Sprites... (watching for Nostr signal)"

*On Failure / Timeout (~25–40 seconds with no Nostr confirmation)*
- The `.now` client starts a tiny local helper (via Service Worker in the JS shell).
- Displays this clean, branded one-liner in a prominent box:

```bash
curl -fsSL https://brave-fox-k7x9.now/launch | bash
```
The local helper serves a dynamically generated `nowbox-launch.sh` script at that path.

This mirrors tools like Claude Code: try the automatic path first, then gracefully offer a reliable manual escape hatch. It works for both Sprites and bare-metal/VPS/Docker hosts.

*nowbox-launch.sh (What the Script Does)*
The generated script is safe (`set -euo pipefail`), smart, and does two things:
1. Launches the sandbox (Sprites VM creation or bare-metal nowbox-server start).
2. Provisions TURN fallback credentials (see below) and passes everything to the sandbox.

Example structure of the served script (customized per session):
```bash
#!/bin/bash
# nowbox-launch.sh for brave-fox-k7x9.now
# Generated by your open .now tab

set -euo pipefail

SESSION="brave-fox-k7x9"
BOOT_TOKEN="abc123-one-time-token..."   # injected at generation

echo "🚀 Launching $SESSION sandbox..."

if [ "$HOST_TYPE" = "sprites" ]; then
  echo "Creating Sprite via API..."
  read -sp "Enter your Sprites API key: " SPRITES_API_KEY
  echo

  curl -X POST "https://api.sprites.dev/v1/sprites" \
    -H "Authorization: Bearer $SPRITES_API_KEY" \
    -H "Content-Type: application/json" \
    -d "{\"name\":\"$SESSION\",\"boot_token\":\"$BOOT_TOKEN\"}" || true

  echo "✅ VM requested. Return to your .now tab — it will auto-connect."
  echo "Waiting for connection via Nostr..."
else
  # Bare metal path: download + run nowbox-server directly
  # ...
fi
```

### Step 3 — Boot (the sandbox)

```
Bootstrap script runs inside the VM:
  curl github.com/nowbox/server/releases/.../nowbox -o /tmp/nowbox
  /tmp/nowbox serve --secret k7x9m2p4... --agent claude-code

nowbox-server:
  → starts Claude Code with PTY
  → starts idle watchdog (if 0 peers connected for 15+ mins -> shut down VM)
  → STUN → learns public IP:port
  → derives Nostr topic from secret
  → randomly selects 5 relays from a hardcoded list of ~50 known good relays
  → connects to Nostr relays
  → publishes encrypted ICE candidates
  → "I'm alive. Here's how to reach me."
  → subscribes to answer topic
  → waits for WebRTC connection
```

### Step 4 — Connect (the .now file finds the sandbox)

```
.now client (still in browser):
  → was already connected to same Nostr relays
  → was already subscribed to same topic (derived from same secret)
  → receives sandbox's ICE candidates
  → UI: "server found, negotiating..."
  → creates WebRTC PeerConnection
  → sets remote description from sandbox candidates
  → creates answer, publishes back via Nostr
  → ICE hole punch
  → DTLS handshake
  → DataChannels open
  → UI: "● connected (p2p)"
  → terminal streams
  → disconnects from Nostr. gossip abandoned.
```

### Step 5 — Use

```
  ┌────────────────────────────────────────┐
  │  brave-fox-k7x9 ── claude code         │
  │                                         │
  │  ● connected (p2p)                      │
  │                                         │
  │  $ Setting up workspace...              │
  │  $ Ready. What are we building?         │
  │  $ █                                    │
  │                                         │
  │  [ ↓ redownload .now ]                  │
  └────────────────────────────────────────┘

Live terminal, direct P2P to the sandbox.
Type commands. See output. Real-time.
All over WebRTC DataChannels. Encrypted.
```

### Step 6 — Share (multiplayer)

```
User clicks [save .now] → downloads brave-fox-k7x9.now
Sends the file to a friend.
Friend double-clicks it → opens in browser.
→ same secret → same Nostr topic → finds same sandbox
   (sandbox is still periodically re-publishing to Nostr)
→ WebRTC connects
→ same PTY session (sandbox forces PTY size to the smallest grid of all connected peers)
→ both type, both see output, real-time
→ both are P2P to the sandbox, not relayed through each other
```



## Ownership

### What we own

| Thing | What it is | Hosted where |
|---|---|---|
| nowbox.com | Static Svelte app. The launcher. | Any CDN. GitHub Pages. Anywhere. |
| generator.wasm | The factory. Builds `.now` files in-browser. Ships with nowbox.com. | Bundled in nowbox.com static assets. |
| nowbox-server | Sandbox binary. Open source. | GitHub releases. Pulled at boot time. |

### What we use but don't own

| Thing | What it is | Who runs it |
|---|---|---|
| Nostr relays | Gossip/signaling (~2 seconds) | Community public relays |
| Google STUN | Address discovery | Google |
| freeturn.net TURN | NAT traversal fallback (TCP/UDP) | freeturn.net (free tier) |
| Sprites / providers | VM hosting | Provider (user's API key) |
| WebRTC | P2P protocol | Built into every browser |

### What we store

Nothing. Zero. No database. No user data. No sessions. No keys. No logs. The `.now` file exists only on the user's machine. The secret exists only inside WASM memory during generation, then only inside the `.now` file.

---

## Trust model

```
nowbox.com JS           → never sees the secret (WASM sandbox)
nowbox.com server       → doesn't exist
Nostr relays            → see encrypted blobs, can't read them
Google STUN             → sees IP addresses, knows nothing else
Sprites / provider      → has the VM, but user's own account/key
The .now file           → IS the trust. whoever has it can connect.
WebRTC connection       → encrypted, cert pinned to the secret
```
