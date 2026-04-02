let ws, role, term, fit, authed = false;

function getToken() {
  const h = location.hash;
  if (!h) return null;
  return new URLSearchParams(h.slice(1)).get('t');
}

// ── Screens ──
function show(id) {
  ['auth-wall','preflight','installing','postflight'].forEach(s =>
    document.getElementById(s).classList.add('hidden'));
  document.getElementById(id).classList.remove('hidden');
}

// ── WebSocket ──
function connect() {
  const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
  ws = new WebSocket(`${proto}//${location.host}/ws`);

  ws.onopen = () => {
    const t = getToken();
    if (!t) {
      document.getElementById('auth-error').textContent = 'no token in URL';
      document.getElementById('auth-error').style.display = '';
      return;
    }
    ws.send(JSON.stringify({type:'auth',token:t}));
  };

  ws.onmessage = e => handle(JSON.parse(e.data));
  ws.onclose = () => { authed = false; setTimeout(connect, 2000); };
}

function send(msg) {
  if (ws && ws.readyState === 1) ws.send(JSON.stringify(msg));
}

// ── Message handler ──
function handle(m) {
  switch (m.type) {
    case 'auth_required': break;
    case 'auth_ok':
      role = m.role;
      authed = true;
      break;
    case 'auth_fail':
      document.getElementById('auth-error').textContent = 'invalid token';
      document.getElementById('auth-error').style.display = '';
      break;
    case 'status':
      onStatus(m.stage);
      break;
    case 'cli':
      if (term) {
        const bytes = Uint8Array.from(atob(m.data), c => c.charCodeAt(0));
        term.write(bytes);
      }
      break;
    case 'share_token':
      showShare(m.token);
      break;
    case 'users':
      document.getElementById('user-count').textContent = m.count;
      break;
  }
}

function onStatus(stage) {
  if (stage === 'preflight') show('preflight');
  else if (stage === 'installing') show('installing');
  else if (stage === 'ready') { show('postflight'); initCLI(); }
  else if (stage === 'exited') { /* could show restart UI */ }
  else if (stage === 'error') { show('preflight'); }
}

// ── CLI (xterm) ──
function initCLI() {
  if (term) return;
  term = new Terminal({
    fontFamily: "'JetBrains Mono', monospace",
    fontSize: 13,
    cursorBlink: true,
    theme: {
      background: '#0a0a0a',
      foreground: '#e0e0e0',
      cursor: '#00ffd5',
      cursorAccent: '#0a0a0a',
      selectionBackground: '#00ffd533',
    }
  });
  fit = new FitAddon.FitAddon();
  term.loadAddon(fit);
  term.open(document.getElementById('cli'));
  fit.fit();

  term.onData(d => {
    const bytes = new TextEncoder().encode(d);
    const b64 = btoa(String.fromCharCode(...bytes));
    send({type:'cli', data: b64});
  });
  term.onResize(({cols, rows}) => send({type:'resize', cols, rows}));
  window.addEventListener('resize', () => fit.fit());

  send({type:'resize', cols: term.cols, rows: term.rows});
  term.focus();
}

// ── Share ──
function showShare(token) {
  document.getElementById('share-url').value =
    `${location.origin}${location.pathname}#t=${token}`;
  document.getElementById('share-modal').classList.remove('hidden');
}

// ── Init ──
document.addEventListener('DOMContentLoaded', () => {
  document.getElementById('share-btn').onclick = () => send({type:'share'});
  document.getElementById('share-copy').onclick = () =>
    navigator.clipboard.writeText(document.getElementById('share-url').value);
  document.getElementById('share-close').onclick = () =>
    document.getElementById('share-modal').classList.add('hidden');

  connect();

  // Re-auth when URL fragment changes (e.g. token added/removed in address bar)
  window.addEventListener('hashchange', () => {
    if (ws) ws.close();
    authed = false;
    role = null;
    show('auth-wall');
    connect();
  });
});
