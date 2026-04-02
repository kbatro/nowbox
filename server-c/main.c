#define _XOPEN_SOURCE 600
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#include "mongoose.h"

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#ifdef __APPLE__
#include <util.h>
#else
#include <pty.h>
#endif

#include "static_files.h"

// ── Tokens ──
#define TOKEN_B58_LEN 44
#define MAX_TOKENS 64

static const char b58_alpha[] =
    "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

typedef enum { ROLE_OWNER, ROLE_COLLABORATOR } role_t;

typedef struct {
  unsigned char hash[32];
  role_t role;
} token_entry_t;

static token_entry_t g_tokens[MAX_TOKENS];
static int g_token_count = 0;
static char g_root_token[TOKEN_B58_LEN + 1];

static void sha256(const void *data, size_t len, unsigned char out[32]) {
  mg_sha256_ctx ctx;
  mg_sha256_init(&ctx);
  mg_sha256_update(&ctx, (const unsigned char *)data, len);
  mg_sha256_final(out, &ctx);
}

static void generate_token(char *out) {
  unsigned char raw[32];
  FILE *f = fopen("/dev/urandom", "r");
  if (f) { fread(raw, 1, 32, f); fclose(f); }
  else for (int i = 0; i < 32; i++) raw[i] = (unsigned char)(rand() & 0xFF);

  unsigned char num[32];
  memcpy(num, raw, 32);
  memset(out, '1', TOKEN_B58_LEN);
  out[TOKEN_B58_LEN] = '\0';

  int idx = TOKEN_B58_LEN - 1;
  for (;;) {
    int rem = 0, zero = 1;
    for (int i = 0; i < 32; i++) {
      int acc = rem * 256 + num[i];
      num[i] = (unsigned char)(acc / 58);
      rem = acc % 58;
      if (num[i]) zero = 0;
    }
    out[idx--] = b58_alpha[rem];
    if (zero || idx < 0) break;
  }
}

static void add_token(const char *tok, role_t role) {
  if (g_token_count >= MAX_TOKENS) return;
  token_entry_t *e = &g_tokens[g_token_count++];
  sha256(tok, strlen(tok), e->hash);
  e->role = role;
}

static int validate_token(const char *tok, role_t *out) {
  unsigned char h[32];
  sha256(tok, strlen(tok), h);
  for (int i = 0; i < g_token_count; i++)
    if (memcmp(g_tokens[i].hash, h, 32) == 0) { *out = g_tokens[i].role; return 1; }
  return 0;
}

// ── Scrollback (ring buffer, replayed to new clients) ──
#define SB_SIZE (256 * 1024)
static unsigned char g_sb[SB_SIZE];
static size_t g_sb_written = 0;

static void sb_append(const unsigned char *d, size_t n) {
  for (size_t i = 0; i < n; i++)
    g_sb[(g_sb_written + i) % SB_SIZE] = d[i];
  g_sb_written += n;
}

// ── PTY ──
static int g_pty_fd = -1;
static pid_t g_child_pid = -1;

static int spawn_pty(const char *cmd) {
  struct winsize ws = {.ws_row = 24, .ws_col = 80};
  g_child_pid = forkpty(&g_pty_fd, NULL, NULL, &ws);
  if (g_child_pid < 0) return -1;
  if (g_child_pid == 0) {
    setenv("TERM", "xterm-256color", 1);
    setenv("LANG", "C.UTF-8", 1);
    setenv("LC_ALL", "C.UTF-8", 1);
    execlp(cmd, cmd, (char *)NULL);
    _exit(1);
  }
  int fl = fcntl(g_pty_fd, F_GETFL, 0);
  fcntl(g_pty_fd, F_SETFL, fl | O_NONBLOCK);
  return 0;
}

// ── Bootstrap state ──
static pid_t g_bootstrap_pid = -1;
static const char *g_agent_cmd_buf = NULL;

// ── Clients ──
#define MAX_CLIENTS 32

typedef struct {
  struct mg_connection *c;
  role_t role;
  int authed;
} client_t;

static client_t g_clients[MAX_CLIENTS];
static int g_ncli = 0;

// preflight = no agent yet, postflight = agent running
static const char *g_status = "preflight";

static client_t *find_cli(struct mg_connection *c) {
  for (int i = 0; i < g_ncli; i++) if (g_clients[i].c == c) return &g_clients[i];
  return NULL;
}

static void add_cli(struct mg_connection *c) {
  if (g_ncli >= MAX_CLIENTS) return;
  g_clients[g_ncli++] = (client_t){.c = c, .authed = 0};
}

static void rm_cli(struct mg_connection *c) {
  for (int i = 0; i < g_ncli; i++)
    if (g_clients[i].c == c) { g_clients[i] = g_clients[--g_ncli]; return; }
}

static void send_all(const char *msg, size_t len) {
  for (int i = 0; i < g_ncli; i++)
    if (g_clients[i].authed) mg_ws_send(g_clients[i].c, msg, len, WEBSOCKET_OP_TEXT);
}

static void send_users(void) {
  int n = 0;
  for (int i = 0; i < g_ncli; i++) if (g_clients[i].authed) n++;
  char buf[64];
  int len = snprintf(buf, sizeof(buf), "{\"type\":\"users\",\"count\":%d}", n);
  send_all(buf, (size_t)len);
}

static void send_status_to(struct mg_connection *c) {
  char buf[64];
  int n = snprintf(buf, sizeof(buf), "{\"type\":\"status\",\"stage\":\"%s\"}", g_status);
  mg_ws_send(c, buf, (size_t)n, WEBSOCKET_OP_TEXT);
}

// ── Base64 ──
static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t b64enc(const unsigned char *in, size_t len, char *out) {
  size_t i, j = 0;
  for (i = 0; i + 2 < len; i += 3) {
    out[j++] = b64[(in[i]>>2)&0x3F];
    out[j++] = b64[((in[i]&3)<<4)|(in[i+1]>>4)];
    out[j++] = b64[((in[i+1]&0xF)<<2)|(in[i+2]>>6)];
    out[j++] = b64[in[i+2]&0x3F];
  }
  if (i < len) {
    out[j++] = b64[(in[i]>>2)&0x3F];
    if (i+1 < len) { out[j++] = b64[((in[i]&3)<<4)|(in[i+1]>>4)]; out[j++] = b64[(in[i+1]&0xF)<<2]; }
    else { out[j++] = b64[(in[i]&3)<<4]; out[j++] = '='; }
    out[j++] = '=';
  }
  out[j] = 0;
  return j;
}

static size_t b64dec(const char *in, size_t len, unsigned char *out) {
  unsigned char dt[256]; memset(dt, 0x80, 256);
  for (int i = 0; i < 64; i++) dt[(unsigned char)b64[i]] = (unsigned char)i;
  dt['='] = 0;
  size_t j = 0; unsigned buf = 0; int bits = 0;
  for (size_t i = 0; i < len; i++) {
    unsigned char c = dt[(unsigned char)in[i]]; if (c == 0x80) continue;
    buf = (buf<<6)|c; bits += 6;
    if (bits >= 8) { bits -= 8; out[j++] = (unsigned char)(buf>>bits); }
  }
  return j;
}

// ── JSON helpers ──
static const char *jstr(const char *json, size_t jlen, const char *key, size_t *vlen) {
  char pat[64]; snprintf(pat, sizeof(pat), "\"%s\":\"", key);
  const char *p = strstr(json, pat);
  if (!p || (size_t)(p-json) >= jlen) return NULL;
  const char *s = p + strlen(pat), *e = strchr(s, '"');
  if (!e) return NULL;
  *vlen = (size_t)(e-s); return s;
}

static int jint(const char *json, size_t jlen, const char *key, int *val) {
  char pat[64]; snprintf(pat, sizeof(pat), "\"%s\":", key);
  const char *p = strstr(json, pat);
  if (!p || (size_t)(p-json) >= jlen) return 0;
  *val = atoi(p + strlen(pat)); return 1;
}

// ── WebSocket message handler ──
static void on_ws_msg(struct mg_connection *c, const char *data, size_t len) {
  client_t *cl = find_cli(c);
  if (!cl) return;

  size_t tlen = 0;
  const char *type = jstr(data, len, "type", &tlen);
  if (!type) return;

  // Auth (must be first message)
  if (!cl->authed) {
    if (tlen == 4 && !memcmp(type, "auth", 4)) {
      size_t tl; const char *tok = jstr(data, len, "token", &tl);
      if (!tok || tl >= 128) return;
      char tb[128]; memcpy(tb, tok, tl); tb[tl] = 0;
      role_t role;
      if (validate_token(tb, &role)) {
        cl->authed = 1; cl->role = role;
        char buf[128];
        int n = snprintf(buf, sizeof(buf), "{\"type\":\"auth_ok\",\"role\":\"%s\"}",
                         role == ROLE_OWNER ? "owner" : "collaborator");
        mg_ws_send(c, buf, (size_t)n, WEBSOCKET_OP_TEXT);
        send_status_to(c);
        send_users();
        // Replay scrollback so new client sees current screen
        if (g_sb_written > 0) {
          size_t total = g_sb_written < SB_SIZE ? g_sb_written : SB_SIZE;
          size_t start = g_sb_written < SB_SIZE ? 0 : (g_sb_written % SB_SIZE);
          size_t sent = 0;
          while (sent < total) {
            size_t chunk = total - sent;
            if (chunk > 3072) chunk = 3072;
            size_t pos = (start + sent) % SB_SIZE;
            size_t contig = SB_SIZE - pos;
            if (chunk > contig) chunk = contig;
            char enc[8192];
            size_t elen = b64enc(g_sb + pos, chunk, enc);
            char msg[16384];
            int mlen = snprintf(msg, sizeof(msg), "{\"type\":\"cli\",\"data\":\"%.*s\"}", (int)elen, enc);
            if (mlen > 0) mg_ws_send(c, msg, (size_t)mlen, WEBSOCKET_OP_TEXT);
            sent += chunk;
          }
        }
      } else {
        mg_ws_send(c, "{\"type\":\"auth_fail\"}", 19, WEBSOCKET_OP_TEXT);
      }
    }
    return;
  }

  // CLI input (keystrokes → PTY)
  if (tlen == 3 && !memcmp(type, "cli", 3)) {
    if (g_pty_fd < 0) return;
    size_t bl; const char *b = jstr(data, len, "data", &bl);
    if (!b) return;
    unsigned char dec[4096];
    size_t dl = b64dec(b, bl, dec);
    if (dl > 0) write(g_pty_fd, dec, dl);
    return;
  }

  // Resize
  if (tlen == 6 && !memcmp(type, "resize", 6)) {
    if (g_pty_fd < 0) return;
    int cols = 80, rows = 24;
    jint(data, len, "cols", &cols); jint(data, len, "rows", &rows);
    struct winsize ws = {.ws_row = (unsigned short)rows, .ws_col = (unsigned short)cols};
    ioctl(g_pty_fd, TIOCSWINSZ, &ws);
    // SIGWINCH forces the agent to redraw — this is how new clients get the screen
    if (g_child_pid > 0) kill(g_child_pid, SIGWINCH);
    return;
  }

  // Share token (owner only)
  if (tlen == 5 && !memcmp(type, "share", 5)) {
    if (cl->role != ROLE_OWNER) return;
    char nt[TOKEN_B58_LEN + 1]; generate_token(nt); add_token(nt, ROLE_COLLABORATOR);
    char buf[128];
    int n = snprintf(buf, sizeof(buf), "{\"type\":\"share_token\",\"token\":\"%s\"}", nt);
    mg_ws_send(c, buf, (size_t)n, WEBSOCKET_OP_TEXT);
    return;
  }

  // Status query
  if (tlen == 6 && !memcmp(type, "status", 6)) { send_status_to(c); return; }

  // Bootstrap: launcher sends a script to execute (owner only, preflight only)
  if (tlen == 9 && !memcmp(type, "bootstrap", 9)) {
    if (cl->role != ROLE_OWNER) return;
    if (strcmp(g_status, "preflight") != 0) return;

    size_t sl; const char *script = jstr(data, len, "script", &sl);
    size_t al; const char *agent = jstr(data, len, "agent", &al);
    if (!script || !agent || sl >= 8192 || al >= 256) return;

    // Copy strings (they'll be freed when JSON is done)
    static char script_buf[8192], agent_buf[256];
    memcpy(script_buf, script, sl); script_buf[sl] = 0;
    memcpy(agent_buf, agent, al); agent_buf[al] = 0;

    g_status = "installing";
    fprintf(stderr, "  bootstrap: installing %s...\n", agent_buf);

    // Notify clients
    char msg[64];
    int n = snprintf(msg, sizeof(msg), "{\"type\":\"status\",\"stage\":\"installing\"}");
    send_all(msg, (size_t)n);

    // Fork and run script
    pid_t pid = fork();
    if (pid == 0) {
      execl("/bin/sh", "sh", "-c", script_buf, (char *)NULL);
      _exit(1);
    }
    // Store for polling
    g_bootstrap_pid = pid;
    g_agent_cmd_buf = agent_buf;
    return;
  }
}

static void check_bootstrap(void) {
  if (g_bootstrap_pid <= 0) return;
  int st;
  pid_t p = waitpid(g_bootstrap_pid, &st, WNOHANG);
  if (p <= 0) return;

  g_bootstrap_pid = -1;

  if (WIFEXITED(st) && WEXITSTATUS(st) == 0) {
    fprintf(stderr, "  bootstrap done, starting agent: %s\n", g_agent_cmd_buf);
    if (spawn_pty(g_agent_cmd_buf) == 0) {
      g_status = "ready";
      fprintf(stderr, "  agent pid: %d\n", g_child_pid);
    } else {
      g_status = "error";
      fprintf(stderr, "  failed to start agent\n");
    }
  } else {
    g_status = "error";
    fprintf(stderr, "  bootstrap failed\n");
  }

  // Notify clients
  char buf[64];
  int n = snprintf(buf, sizeof(buf), "{\"type\":\"status\",\"stage\":\"%s\"}", g_status);
  send_all(buf, (size_t)n);
}

// ── HTTP + WS event handler ──
static void ev_handler(struct mg_connection *c, int ev, void *ev_data) {
  if (ev == MG_EV_HTTP_MSG) {
    struct mg_http_message *hm = (struct mg_http_message *)ev_data;

    if (mg_match(hm->uri, mg_str("/ws"), NULL)) {
      mg_ws_upgrade(c, hm, NULL);
      add_cli(c);
      mg_ws_send(c, "{\"type\":\"auth_required\"}", 23, WEBSOCKET_OP_TEXT);
      return;
    }

    if (mg_match(hm->uri, mg_str("/style.css"), NULL)) {
      mg_http_reply(c, 200, "Content-Type: text/css\r\nCache-Control: no-cache\r\n",
                    "%.*s", (int)static_style_css_len, static_style_css);
      return;
    }

    if (mg_match(hm->uri, mg_str("/app.js"), NULL)) {
      mg_http_reply(c, 200, "Content-Type: application/javascript\r\nCache-Control: no-cache\r\n",
                    "%.*s", (int)static_app_js_len, static_app_js);
      return;
    }

    mg_http_reply(c, 200, "Content-Type: text/html\r\nCache-Control: no-cache\r\n",
                  "%.*s", (int)static_index_html_len, static_index_html);
    return;
  }

  if (ev == MG_EV_WS_MSG) {
    struct mg_ws_message *wm = (struct mg_ws_message *)ev_data;
    on_ws_msg(c, wm->data.buf, wm->data.len);
    return;
  }

  if (ev == MG_EV_CLOSE) {
    int was_authed = 0;
    client_t *cl = find_cli(c);
    if (cl) was_authed = cl->authed;
    rm_cli(c);
    if (was_authed) send_users();
  }
}

// ── Nostr + WebRTC signaling ──
#include "nostr.h"
#include "webrtc.h"

static nostr_ctx_t g_nostr;
static webrtc_ctx_t g_webrtc;
static int g_nostr_enabled = 0;
static time_t g_nostr_last_publish = 0;

// ── PTY → WebSocket + WebRTC broadcast ──
static void poll_pty(void) {
  if (g_pty_fd < 0) return;
  unsigned char buf[4096];
  ssize_t n = read(g_pty_fd, buf, sizeof(buf));
  if (n <= 0) return;
  sb_append(buf, (size_t)n);

  char enc[8192];
  size_t elen = b64enc(buf, (size_t)n, enc);
  char msg[16384];
  int mlen = snprintf(msg, sizeof(msg), "{\"type\":\"cli\",\"data\":\"%.*s\"}", (int)elen, enc);
  if (mlen > 0 && (size_t)mlen < sizeof(msg)) {
    send_all(msg, (size_t)mlen);
    // Also send over WebRTC DataChannel
    if (g_webrtc.connected) {
      int rc = webrtc_send(&g_webrtc, msg, mlen);
      fprintf(stderr, "  webrtc: sent %d bytes (rc=%d)\n", mlen, rc);
    }
  }
}

static void on_webrtc_open(void *userdata) {
  (void)userdata;
  fprintf(stderr, "  → WebRTC peer connected\n");
  // Don't send data from this callback (runs on libdatachannel thread).
  // PTY data will flow via poll_pty on the main thread.
}

static void on_webrtc_message(const char *data, int len, void *userdata) {
  (void)userdata;

  // Parse message type
  size_t tlen = 0;
  const char *type = jstr(data, (size_t)len, "type", &tlen);
  if (!type) return;

  // CLI input (keystrokes → PTY)
  if (tlen == 3 && !memcmp(type, "cli", 3)) {
    if (g_pty_fd < 0) return;
    size_t bl; const char *b = jstr(data, (size_t)len, "data", &bl);
    if (!b) return;
    unsigned char dec[4096];
    size_t dl = b64dec(b, bl, dec);
    if (dl > 0) write(g_pty_fd, dec, dl);
    return;
  }

  // Resize
  if (tlen == 6 && !memcmp(type, "resize", 6)) {
    if (g_pty_fd < 0) return;
    int cols = 80, rows = 24;
    jint(data, (size_t)len, "cols", &cols);
    jint(data, (size_t)len, "rows", &rows);
    struct winsize ws = {.ws_row = (unsigned short)rows, .ws_col = (unsigned short)cols};
    ioctl(g_pty_fd, TIOCSWINSZ, &ws);
    if (g_child_pid > 0) kill(g_child_pid, SIGWINCH);
    return;
  }
}

static void on_webrtc_close(void *userdata) {
  (void)userdata;
  fprintf(stderr, "  → WebRTC peer disconnected\n");
}

static void on_nostr_signal(nostr_signal_t *msg, void *userdata) {
  (void)userdata;
  webrtc_on_signal(&g_webrtc, msg);
}

// ── Main ──
int main(int argc, char *argv[]) {
  signal(SIGPIPE, SIG_IGN);

  const char *port = getenv("NOWBOX_PORT");
  if (!port) port = "8080";

  const char *agent = getenv("NOWBOX_AGENT");
  const char *bootstrap = getenv("NOWBOX_BOOTSTRAP");
  const char *secret = NULL;

  // Parse --secret flag
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--secret") == 0 && i + 1 < argc) {
      secret = argv[++i];
    }
  }

  generate_token(g_root_token);
  add_token(g_root_token, ROLE_OWNER);

  fprintf(stderr, "\n  nowbox-server :%s\n", port);
  fprintf(stderr, "  token: %s\n", g_root_token);

  if (bootstrap && agent) {
    fprintf(stderr, "  mode: bootstrap\n");
    g_status = "installing";
    g_agent_cmd_buf = agent;
    g_bootstrap_pid = fork();
    if (g_bootstrap_pid == 0) {
      execl("/bin/sh", "sh", "-c", bootstrap, (char *)NULL);
      _exit(1);
    }
  } else if (agent) {
    fprintf(stderr, "  mode: direct (%s)\n", agent);
    if (spawn_pty(agent) < 0) { fprintf(stderr, "  spawn failed\n"); return 1; }
    g_status = "ready";
  } else {
    fprintf(stderr, "  mode: preflight (waiting for bootstrap)\n");
    g_status = "preflight";
  }

  struct mg_mgr mgr;
  mg_mgr_init(&mgr);

  // Initialize Nostr + WebRTC signaling if secret provided
  if (secret) {
    fprintf(stderr, "\n  nostr: initializing...\n");
    if (nostr_init(&g_nostr, &mgr, secret) == 0) {
      g_nostr_enabled = 1;
      fprintf(stderr, "  nostr: topic: %.16s...\n", g_nostr.topic);
      fprintf(stderr, "  nostr: pubkey: %.16s...\n", g_nostr.pubkey_hex);
      nostr_set_handler(&g_nostr, on_nostr_signal, NULL);
      nostr_connect(&g_nostr);

      fprintf(stderr, "\n  webrtc: initializing...\n");
      webrtc_set_callbacks(&g_webrtc, on_webrtc_open, on_webrtc_message,
                           on_webrtc_close, NULL);
      if (webrtc_init(&g_webrtc, &g_nostr) < 0) {
        fprintf(stderr, "  webrtc: init failed\n");
      }
    } else {
      fprintf(stderr, "  nostr: init failed\n");
    }
  }

  char url[64];
  snprintf(url, sizeof(url), "http://0.0.0.0:%s", port);
  mg_http_listen(&mgr, url, ev_handler, NULL);
  fprintf(stderr, "  listening...\n\n");
  setbuf(stderr, NULL);

  for (;;) {
    mg_mgr_poll(&mgr, 10);
    check_bootstrap();
    poll_pty();

    // Re-publish WebRTC offer every 5 seconds (so late-joining clients find us)
    if (g_nostr_enabled && !g_webrtc.connected) {
      time_t now = time(NULL);
      if (now - g_nostr_last_publish >= 5) {
        g_nostr_last_publish = now;
        webrtc_republish_offer(&g_webrtc);
      }
    }

    if (g_pty_fd >= 0) {
      int st;
      pid_t p = waitpid(g_child_pid, &st, WNOHANG);
      if (p > 0) {
        fprintf(stderr, "  agent exited (%d)\n", WEXITSTATUS(st));
        close(g_pty_fd);
        g_pty_fd = -1;
        g_status = "exited";
        char buf[64];
        int n = snprintf(buf, sizeof(buf), "{\"type\":\"status\",\"stage\":\"exited\"}");
        send_all(buf, (size_t)n);
      }
    }
  }
}
