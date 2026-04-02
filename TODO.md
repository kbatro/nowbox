# nowbox TODO

## Next: Multi-thread session management
- Token = the machine, not the process
- Server manages multiple threads (agent processes)
- **Lobby view**: lists running + exited threads, start new
- **In a thread**:
  - `+ new` → creates another thread on same machine, switches to it
  - agent exits → thread closes, back to lobby
  - `back` → return to lobby, thread keeps running in background
  - `nowbox logo` → link to Svelte launcher (new machine entirely)
- Launcher (Svelte) creates machine + first thread in one shot (user drops straight into agent)
- Collaborators with share token see same lobby, can join running threads

## Future
- Svelte landing page (launcher)
- Multiple providers: Sprites, E2B, Cloudflare, AWS
- Provider adapters generate bootstrap scripts per platform
