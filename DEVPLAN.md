# nowbox — dev plan

## Wave 1: Proof of Life (WebRTC + Nostr signaling)

The core question: can two machines find each other via Nostr and connect via WebRTC? Everything else is built on top of this.

### 1.1 — nowbox-server: Nostr + WebRTC in a binary

- Language: Go (pion/webrtc is mature, nkn/nostr or nbd-wtf/go-nostr for Nostr)
- Accept a `--secret` flag
- Derive Nostr keypair + topic from secret
- Connect to 3 hardcoded Nostr relays
- Publish ICE candidates as encrypted ephemeral event
- Listen for WebRTC answer via Nostr
- Complete WebRTC handshake
- Open a DataChannel, echo anything received (echo server)
- Print "peer connected" when DataChannel opens
- **Deliverable**: a binary you run with `./nowbox-server --secret test123` that sits there waiting for a WebRTC peer

### 1.2 — Browser client: Nostr + WebRTC in vanilla JS

- Single HTML file, no build step, no framework
- Hardcode a secret (same as server for testing)
- Connect to same Nostr relays
- Subscribe to derived topic
- When server's ICE candidates arrive, create RTCPeerConnection
- Exchange answer via Nostr
- Open DataChannel, send "hello", display anything received
- **Deliverable**: open the HTML file, it finds the server and connects. Type in a box, see echo.

### 1.3 — Wire them together

- Run server on a VPS or local machine
- Open the HTML file in browser
- Verify: Nostr signaling works, WebRTC connects, DataChannel echoes
- Test across networks (home → VPS, coffee shop → VPS)
- Measure: time from page open to connected
- **Gate**: if this works reliably across 3+ different networks, proceed

---

## Wave 2: Terminal Streaming

### 2.1 — PTY on the server

- Server spawns a shell (`/bin/bash` or declared agent) inside a PTY
- PTY output → DataChannel (`tty` channel)
- DataChannel input → PTY stdin
- Handle PTY resize via `ctrl` DataChannel
- Handle process exit cleanly (notify client, allow restart)

### 2.2 — Terminal in the browser

- Add xterm.js to the HTML client
- Render DataChannel `tty` output in xterm.js
- Send keyboard input over DataChannel
- Handle resize: send terminal dimensions on connect + on window resize
- **Deliverable**: open the HTML file, get a live terminal to the server. Type commands, see output.

### 2.3 — Agent bootstrapping

- Server accepts `--agent` flag
- Downloads and starts the declared agent (Claude Code, etc.) instead of bare shell
- Agent runs inside the PTY
- **Deliverable**: `./nowbox-server --secret test123 --agent claude-code` → browser shows Claude Code running

---

## Wave 3: The `.now` File

### 3.1 — Define the `.now` format

- HTML file with embedded config as a JSON blob in a `<script type="application/json">` tag
- Config: `{ secret, host, agent, relays, created }`
- The client JS reads the config on load
- No WASM yet — just JS. WASM comes in wave 5.
- **Deliverable**: a template HTML file where you swap in the config JSON and it becomes a working `.now` client

### 3.2 — `.now` file generator (JS, no WASM yet)

- A function that takes (host, agent) and returns a complete `.now` HTML string
- Generates the secret client-side (`crypto.getRandomValues`)
- Injects config into the template
- Returns as a Blob
- **Deliverable**: `generateNowFile("sprites", "claude-code")` → returns a Blob you can download or open

### 3.3 — File behaviors

- Test: open from `file://` — does WebRTC work? Does Nostr WebSocket work?
- Test: open via blob URL — same checks
- Test: double-click on macOS, Windows, Linux — does it open in browser?
- Register `.now` → `text/html` MIME type (document how for each OS)
- Add "save .now" / "redownload .now" button inside the connected UI
- **Deliverable**: a `.now` file that works from `file://`, blob URL, and double-click

---

## Wave 4: nowbox.com (the launcher)

### 4.1 — Svelte app scaffold

- SvelteKit with adapter-static
- Single page, two-mode design based on presence of config
- No routing needed — everything is one page
- Brutalist design, monospace, dark

### 4.2 — Launcher UI

- Form: host type dropdown (Sprites first, others later), agent dropdown
- "Go" button
- On click: generate `.now` file (using the JS generator from 3.2)
- Trigger download + open blob URL in new tab
- Show: "your .now file has been generated. check your downloads."
- The page is done. No state. No tracking.

### 4.3 — Provider adapters

- Sprites adapter: generate a bootstrap script that curls nowbox-server and runs it
- The bootstrap script includes the secret
- The `.now` client handles the API call to Sprites (or shows the curl fallback)
- **Deliverable**: click Go on nowbox.com → `.now` file opens → enter Sprites key → VM boots → terminal connected

---

## Wave 5: WASM Hardening

### 5.1 — Client WASM module

- Rewrite the client logic (Nostr + WebRTC signaling + encryption) in Rust → compile to WASM
- The WASM handles: secret derivation, Nostr protocol, ICE candidate formatting, encryption/decryption
- JS shell provides: WebSocket, RTCPeerConnection, DOM rendering
- Terminal rendering stays in JS (xterm.js)
- **Deliverable**: `.now` file client logic runs in WASM, JS shell is just glue

### 5.2 — generator.wasm

- Rewrite the `.now` file generator in Rust → compile to WASM
- Secret generation happens in WASM linear memory
- Assembles the `.now` HTML with embedded client WASM
- Returns opaque blob to JS
- JS never touches the secret
- **Deliverable**: nowbox.com calls generator.wasm, gets back a `.now` blob, opens it

### 5.3 — Deterministic builds

- CI pipeline that builds generator.wasm and client.wasm reproducibly
- Published hash so anyone can verify the WASM matches source
- **Deliverable**: `sha256(generator.wasm)` in the README, verifiable by anyone

---

## Wave 6: Multiplayer + Polish

### 6.1 — Multiple peers

- Server accepts multiple WebRTC connections (same secret = authorized)
- All peers see the same PTY output
- All peers can type (input is interleaved)
- PTY resizes to smallest grid of all connected peers
- Server re-publishes to Nostr periodically so new peers can find it

### 6.2 — Idle watchdog

- Server tracks connected peer count
- 0 peers for 15 minutes → clean shutdown
- If hosted on Sprites: shutdown triggers VM destruction (via provider API or just process exit)
- Watchdog timer resets on each peer connect/disconnect

### 6.3 — Connection resilience

- Client auto-reconnects if WebRTC drops (re-does Nostr signaling)
- Server handles peer disconnect gracefully (PTY keeps running)
- ICE restart on network change (laptop switches wifi → mobile)

### 6.4 — TURN fallback

- Add freeturn.net (or similar) to ICE servers config
- Server includes iceServers array in Nostr announcement
- Client uses whatever the server provides
- Test on restrictive networks (corporate, hotel wifi)

---

## Wave 7: More Providers + Hosts

### 7.1 — Provider adapter interface

- Define the adapter interface: `createVM(config) → { id, bootstrap_method }`
- Each adapter generates a provider-specific bootstrap script
- The `.now` client handles the provider API call (or curl fallback)

### 7.2 — Additional providers

- Docker/Podman (local container)
- Generic SSH (any box you can SSH into)
- E2B
- Fly.io
- AWS/GCP (via their CLI tools in the curl fallback)

### 7.3 — Local containers (Apple Virtualization / Podman)

- `nowbox up --local` or a "Local" option in the launcher
- Spins up a lightweight Linux VM/container on the user's machine
- The `.now` file connects to localhost (no NAT traversal needed)
- Good for development, testing, offline use

---

## Wave 8: MCP Bridge (v2)

### 8.1 — MCP over DataChannel

- Add `mcp` DataChannel to the protocol
- Server exposes agent tools via MCP JSON-RPC over the DataChannel
- Browser UI can call MCP tools (file read, terminal command, etc.)

### 8.2 — Node.js MCP bridge

- `nowbox-run` CLI tool (or npx package)
- Takes a `.now` file as input
- Runs the client WASM in Node
- Bridges stdio ↔ WebRTC DataChannel
- Claude Code config: `{ "command": "nowbox-run", "args": ["brave-fox-k7x9.now"] }`
- **Deliverable**: Claude Code locally talks MCP to a remote sandbox via WebRTC

---

## Build order summary

```
Wave 1: proof of life           → can two machines connect via Nostr + WebRTC?
Wave 2: terminal streaming      → can I get a live shell over that connection?
Wave 3: the .now file           → can I package it into a self-contained file?
Wave 4: nowbox.com              → can I generate .now files from a nice UI?
Wave 5: WASM hardening          → can I isolate secrets in WASM?
Wave 6: multiplayer + polish    → can multiple people share a session?
Wave 7: more providers          → can I run this on anything?
Wave 8: MCP bridge              → can Claude Code talk to the sandbox remotely?
```

Each wave is independently demoable. Each wave builds on the previous. Wave 1 is the riskiest (does the Nostr + WebRTC dance actually work reliably?) — everything after is incremental.
