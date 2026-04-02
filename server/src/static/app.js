// ── State ──
let ws = null;
let role = null;
let term = null;
let fitAddon = null;
let currentView = 'loading';
let termBuffer = '';

// ── Token from URL fragment ──
function getToken() {
  const hash = window.location.hash;
  if (!hash) return null;
  const params = new URLSearchParams(hash.slice(1));
  return params.get('t');
}

// ── WebSocket ──
function connect() {
  const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
  ws = new WebSocket(`${proto}//${location.host}/ws`);

  ws.onopen = () => {
    const token = getToken();
    if (!token) {
      document.getElementById('auth-error').textContent = 'no token in URL';
      document.getElementById('auth-error').style.display = '';
      return;
    }
    ws.send(JSON.stringify({ type: 'auth', token }));
  };

  ws.onmessage = (e) => {
    const msg = JSON.parse(e.data);
    handleMessage(msg);
  };

  ws.onclose = () => {
    setTimeout(connect, 2000);
  };
}

function send(msg) {
  if (ws && ws.readyState === WebSocket.OPEN) {
    ws.send(JSON.stringify(msg));
  }
}

// ── Message handler ──
function handleMessage(msg) {
  switch (msg.type) {
    case 'auth_required':
      break; // we already send auth on open

    case 'auth_ok':
      role = msg.role;
      document.getElementById('auth-wall').classList.add('hidden');
      break;

    case 'auth_fail':
      document.getElementById('auth-error').textContent = 'invalid token';
      document.getElementById('auth-error').style.display = '';
      break;

    case 'status':
      updateStatus(msg.stage);
      break;

    case 'terminal':
      handleTerminalData(msg.data);
      break;

    case 'chat':
      addChatMessage(msg.from || 'agent', msg.content);
      break;

    case 'share_token':
      showShareModal(msg.token);
      break;

    case 'users':
      document.getElementById('user-count').textContent = msg.count;
      break;

    case 'error':
      console.error('server:', msg.message);
      break;
  }
}

// ── Status / Loading ──
const stageOrder = ['booting', 'installing_agent', 'starting_agent', 'ready'];

function updateStatus(stage) {
  const idx = stageOrder.indexOf(stage);
  document.querySelectorAll('.loading-stage').forEach((el, i) => {
    el.classList.remove('done', 'active');
    if (i < idx) el.classList.add('done');
    if (i === idx) el.classList.add('active');
  });

  if (stage === 'ready') {
    document.getElementById('loading-status').textContent = 'ready';
    setTimeout(() => switchView('chat'), 500);
  }
}

// ── View switching ──
function switchView(name) {
  currentView = name;
  document.querySelectorAll('.view').forEach(v => v.classList.remove('active'));
  document.querySelectorAll('.view-tab').forEach(t => t.classList.remove('active'));

  const view = document.getElementById(`view-${name}`);
  if (view) view.classList.add('active');

  const tab = document.querySelector(`.view-tab[data-view="${name}"]`);
  if (tab) tab.classList.add('active');

  if (name === 'terminal') {
    initTerminal();
    if (fitAddon) fitAddon.fit();
  }
}

// ── Terminal ──
function initTerminal() {
  if (term) return;

  term = new Terminal({
    fontFamily: "'JetBrains Mono', monospace",
    fontSize: 13,
    theme: {
      background: '#0a0a0a',
      foreground: '#e0e0e0',
      cursor: '#00ffd5',
      cursorAccent: '#0a0a0a',
      selectionBackground: '#00ffd533',
    },
    cursorBlink: true,
    allowProposedApi: true,
  });

  fitAddon = new FitAddon.FitAddon();
  term.loadAddon(fitAddon);
  term.open(document.getElementById('terminal-container'));
  fitAddon.fit();

  // Write any buffered data
  if (termBuffer) {
    term.write(termBuffer);
    termBuffer = '';
  }

  // Send keystrokes
  term.onData((data) => {
    send({ type: 'terminal', data: btoa(data) });
  });

  // Resize
  term.onResize(({ cols, rows }) => {
    send({ type: 'resize', cols, rows });
  });

  window.addEventListener('resize', () => {
    if (currentView === 'terminal' && fitAddon) fitAddon.fit();
  });

  // Send initial size
  send({ type: 'resize', cols: term.cols, rows: term.rows });
}

function handleTerminalData(b64) {
  const data = atob(b64);
  if (term) {
    term.write(data);
  } else {
    termBuffer += data;
  }
}

// ── Chat ──
function addChatMessage(from, content) {
  const container = document.getElementById('chat-messages');
  const el = document.createElement('div');
  el.className = `chat-msg ${from === 'user' ? 'user' : 'agent'}`;
  el.textContent = content;
  container.appendChild(el);
  container.scrollTop = container.scrollHeight;
}

function sendChat() {
  const input = document.getElementById('chat-input');
  const msg = input.value.trim();
  if (!msg) return;
  addChatMessage('user', msg);
  send({ type: 'chat', message: msg });
  input.value = '';
}

// ── Share ──
function showShareModal(token) {
  const url = `${location.origin}${location.pathname}#t=${token}`;
  document.getElementById('share-url').value = url;
  document.getElementById('share-modal').classList.remove('hidden');
}

// ── Init ──
document.addEventListener('DOMContentLoaded', () => {
  // View tabs
  document.querySelectorAll('.view-tab').forEach(tab => {
    tab.addEventListener('click', () => switchView(tab.dataset.view));
  });

  // Chat
  document.getElementById('chat-send').addEventListener('click', sendChat);
  document.getElementById('chat-input').addEventListener('keydown', (e) => {
    if (e.key === 'Enter') sendChat();
  });

  // Share
  document.getElementById('share-btn').addEventListener('click', () => {
    send({ type: 'create_share_token' });
  });
  document.getElementById('share-copy').addEventListener('click', () => {
    const url = document.getElementById('share-url');
    navigator.clipboard.writeText(url.value);
  });
  document.getElementById('share-close').addEventListener('click', () => {
    document.getElementById('share-modal').classList.add('hidden');
  });

  // Keyboard shortcut: Ctrl+` to toggle views
  document.addEventListener('keydown', (e) => {
    if (e.ctrlKey && e.key === '`') {
      e.preventDefault();
      switchView(currentView === 'chat' ? 'terminal' : 'chat');
    }
  });

  // Connect
  connect();
});
