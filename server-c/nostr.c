#include "nostr.h"

#include <secp256k1.h>
#include <secp256k1_ecdh.h>
#include <secp256k1_extrakeys.h>
#include <secp256k1_schnorrsig.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

// ── Hex helpers ──

static void to_hex(const unsigned char *in, size_t len, char *out) {
  static const char hex[] = "0123456789abcdef";
  for (size_t i = 0; i < len; i++) {
    out[i * 2] = hex[in[i] >> 4];
    out[i * 2 + 1] = hex[in[i] & 0xf];
  }
  out[len * 2] = '\0';
}

static int from_hex(const char *in, size_t hexlen, unsigned char *out) {
  for (size_t i = 0; i < hexlen / 2; i++) {
    unsigned int b;
    if (sscanf(in + i * 2, "%2x", &b) != 1) return -1;
    out[i] = (unsigned char)b;
  }
  return 0;
}

// ── SHA-256 (using Mongoose's built-in) ──

static void sha256(const void *data, size_t len, unsigned char out[32]) {
  mg_sha256_ctx ctx;
  mg_sha256_init(&ctx);
  mg_sha256_update(&ctx, (const unsigned char *)data, len);
  mg_sha256_final(out, &ctx);
}

// ── AES-256-CBC for NIP-04 ──
// Minimal AES implementation for NIP-04 encryption/decryption
// NIP-04 uses AES-256-CBC with PKCS7 padding

// We'll use a simple AES-256-CBC from a small embedded implementation
// For now, use OpenSSL if available, otherwise skip encryption in wave1
// and send plaintext (relays can see it, but it works for testing)

// TODO: Add proper AES-256-CBC for NIP-04. For wave1, we'll skip
// encryption and use a simpler approach: the topic hash is unguessable
// enough for testing. Production will need NIP-04.

// ── Nostr event JSON building ──

static int nostr_event_id(const char *pubkey_hex, int64_t created_at,
                          int kind, const char *tags_json,
                          const char *content_escaped, unsigned char id_out[32]) {
  // Event ID = SHA-256 of the canonical serialization:
  // [0,"<pubkey>",<created_at>,<kind>,<tags>,"<content>"]
  // content_escaped must already be JSON-escaped (backslashes, quotes, newlines)
  char buf[NOSTR_MAX_MSG * 2];
  int n = snprintf(buf, sizeof(buf),
                   "[0,\"%s\",%lld,%d,%s,\"%s\"]",
                   pubkey_hex, (long long)created_at, kind, tags_json, content_escaped);
  if (n < 0 || (size_t)n >= sizeof(buf)) return -1;
  sha256(buf, (size_t)n, id_out);
  return 0;
}

static int nostr_sign_event(const unsigned char privkey[32],
                            const unsigned char event_id[32],
                            unsigned char sig_out[64]) {
  secp256k1_context *sctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN);
  secp256k1_keypair keypair;

  if (!secp256k1_keypair_create(sctx, &keypair, privkey)) {
    secp256k1_context_destroy(sctx);
    return -1;
  }

  if (!secp256k1_schnorrsig_sign32(sctx, sig_out, event_id, &keypair, NULL)) {
    secp256k1_context_destroy(sctx);
    return -1;
  }

  secp256k1_context_destroy(sctx);
  return 0;
}

// ── Key derivation ──

int nostr_init(nostr_ctx_t *ctx, struct mg_mgr *mgr, const char *secret) {
  memset(ctx, 0, sizeof(*ctx));
  ctx->mgr = mgr;

  // Private key = SHA-256(secret)
  sha256(secret, strlen(secret), ctx->privkey);
  to_hex(ctx->privkey, 32, ctx->privkey_hex);

  // Public key (x-only, 32 bytes) from secp256k1
  secp256k1_context *sctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN);
  secp256k1_keypair keypair;
  if (!secp256k1_keypair_create(sctx, &keypair, ctx->privkey)) {
    secp256k1_context_destroy(sctx);
    return -1;
  }

  secp256k1_xonly_pubkey xpub;
  secp256k1_keypair_xonly_pub(sctx, &xpub, NULL, &keypair);
  secp256k1_xonly_pubkey_serialize(sctx, ctx->pubkey, &xpub);
  to_hex(ctx->pubkey, 32, ctx->pubkey_hex);

  // ECDH shared secret (for NIP-04, encrypting to self)
  secp256k1_pubkey full_pubkey;
  secp256k1_keypair_pub(sctx, &full_pubkey, &keypair);
  (void)secp256k1_ecdh(sctx, ctx->shared_secret, &full_pubkey, ctx->privkey, NULL, NULL);

  secp256k1_context_destroy(sctx);

  // Topic = SHA-256("nowbox-topic:" + secret)
  char topic_input[256];
  snprintf(topic_input, sizeof(topic_input), "nowbox-topic:%s", secret);
  unsigned char topic_hash[32];
  sha256(topic_input, strlen(topic_input), topic_hash);
  to_hex(topic_hash, 32, ctx->topic);

  // Default relays
  ctx->relay_urls[0] = "wss://relay.damus.io";
  ctx->relay_urls[1] = "wss://nos.lol";
  ctx->relay_urls[2] = "wss://relay.nostr.band";
  ctx->relay_count = 3;

  return 0;
}

// ── Relay WebSocket event handler ──

static void escape_json_string(const char *in, char *out, size_t out_size) {
  size_t j = 0;
  for (size_t i = 0; in[i] && j < out_size - 2; i++) {
    if (in[i] == '"' || in[i] == '\\') {
      out[j++] = '\\';
    }
    if (in[i] == '\n') {
      out[j++] = '\\';
      out[j++] = 'n';
      continue;
    }
    if (in[i] == '\r') {
      out[j++] = '\\';
      out[j++] = 'r';
      continue;
    }
    out[j++] = in[i];
  }
  out[j] = '\0';
}

static int parse_signal_message(const char *json, size_t len, nostr_signal_t *msg) {
  // Minimal JSON parser for our signal messages
  memset(msg, 0, sizeof(*msg));

  // Find "type":"..."
  const char *t = strstr(json, "\"type\":\"");
  if (!t || (size_t)(t - json) >= len) return -1;
  t += 8;

  if (strncmp(t, "offer", 5) == 0) msg->type = NOSTR_MSG_OFFER;
  else if (strncmp(t, "answer", 6) == 0) msg->type = NOSTR_MSG_ANSWER;
  else if (strncmp(t, "candidate", 9) == 0) msg->type = NOSTR_MSG_CANDIDATE;
  else return -1;

  // Find "sdp":"..." and unescape JSON string
  const char *s = strstr(json, "\"sdp\":\"");
  if (s && (size_t)(s - json) < len) {
    s += 7;
    size_t j = 0;
    while (*s && *s != '"' && j < sizeof(msg->sdp) - 1) {
      if (*s == '\\' && *(s + 1)) {
        s++;
        if (*s == 'n') msg->sdp[j++] = '\n';
        else if (*s == 'r') msg->sdp[j++] = '\r';
        else if (*s == 't') msg->sdp[j++] = '\t';
        else if (*s == '\\') msg->sdp[j++] = '\\';
        else if (*s == '"') msg->sdp[j++] = '"';
        else msg->sdp[j++] = *s;
      } else {
        msg->sdp[j++] = *s;
      }
      s++;
    }
    msg->sdp[j] = '\0';
  }

  // Find "candidate":"..."
  const char *c = strstr(json, "\"candidate\":\"");
  if (c && (size_t)(c - json) < len) {
    c += 13;
    const char *e = c;
    while (*e && *e != '"') {
      if (*e == '\\') e++;
      e++;
    }
    size_t clen = (size_t)(e - c);
    if (clen < sizeof(msg->candidate)) {
      memcpy(msg->candidate, c, clen);
      msg->candidate[clen] = '\0';
    }
  }

  // Find "sdpMid":"..."
  const char *m = strstr(json, "\"sdpMid\":\"");
  if (m && (size_t)(m - json) < len) {
    m += 10;
    const char *e = strchr(m, '"');
    if (e) {
      size_t mlen = (size_t)(e - m);
      if (mlen < sizeof(msg->sdp_mid)) {
        memcpy(msg->sdp_mid, m, mlen);
        msg->sdp_mid[mlen] = '\0';
      }
    }
  }

  // Find "sdpMLineIndex":N
  const char *idx = strstr(json, "\"sdpMLineIndex\":");
  if (idx && (size_t)(idx - json) < len) {
    msg->sdp_mline_index = atoi(idx + 16);
  }

  return 0;
}

static void relay_handler(struct mg_connection *c, int ev, void *ev_data) {
  nostr_ctx_t *ctx = (nostr_ctx_t *)c->fn_data;

  if (ev == MG_EV_CONNECT) {
    // Initialize TLS for wss:// connections
    if (c->is_tls) {
      struct mg_tls_opts opts = {0};
      mg_tls_init(c, &opts);
    }
  }

  if (ev == MG_EV_WS_OPEN) {
    fprintf(stderr, "  nostr: connected to relay\n");

    // Subscribe: ["REQ", "sub1", {"kinds":[20100], "#d":["<topic>"]}]
    char sub[512];
    snprintf(sub, sizeof(sub),
             "[\"REQ\",\"sub1\",{\"kinds\":[20100],\"#d\":[\"%s\"]}]",
             ctx->topic);
    mg_ws_send(c, sub, strlen(sub), WEBSOCKET_OP_TEXT);
  }

  if (ev == MG_EV_WS_MSG) {
    struct mg_ws_message *wm = (struct mg_ws_message *)ev_data;
    const char *data = wm->data.buf;
    size_t len = wm->data.len;

    // Handle OK responses: ["OK", "event_id", true/false, "message"]
    if (len > 5 && strncmp(data, "[\"OK\"", 5) == 0) {
      fprintf(stderr, "  nostr: relay response: %.*s\n",
              (int)(len > 200 ? 200 : len), data);
      return;
    }

    // Handle NOTICE: ["NOTICE", "message"]
    if (len > 9 && strncmp(data, "[\"NOTICE\"", 9) == 0) {
      fprintf(stderr, "  nostr: relay notice: %.*s\n",
              (int)(len > 200 ? 200 : len), data);
      return;
    }

    // Handle EOSE: ["EOSE", "sub_id"]
    if (len > 7 && strncmp(data, "[\"EOSE\"", 7) == 0) {
      fprintf(stderr, "  nostr: end of stored events\n");
      return;
    }

    // Parse Nostr relay message: ["EVENT", "sub1", {...event...}]
    if (len < 10 || strncmp(data, "[\"EVENT\"", 8) != 0) return;

    // Extract event content field
    // Find "content":"..." in the event
    const char *content_key = strstr(data, "\"content\":\"");
    if (!content_key) return;
    content_key += 11;

    // Find end of content string (handle escapes)
    const char *end = content_key;
    while (*end && *end != '"') {
      if (*end == '\\') end++;
      end++;
    }
    size_t content_len = (size_t)(end - content_key);

    // Skip events older than 30 seconds
    const char *ts_key = strstr(data, "\"created_at\":");
    if (ts_key) {
      int64_t event_ts = (int64_t)atoll(ts_key + 13);
      int64_t now_ts = (int64_t)time(NULL);
      if (now_ts - event_ts > 30) return;
    }

    // Extract event ID for dedup
    const char *id_key = strstr(data, "\"id\":\"");
    if (id_key) {
      id_key += 6;
      char event_id[65];
      size_t id_len = 0;
      while (id_key[id_len] && id_key[id_len] != '"' && id_len < 64) id_len++;
      memcpy(event_id, id_key, id_len);
      event_id[id_len] = '\0';

      // Dedup check
      for (int i = 0; i < 64; i++) {
        if (strcmp(ctx->seen_ids[i], event_id) == 0) return;
      }
      strncpy(ctx->seen_ids[ctx->seen_idx % 64], event_id, 64);
      ctx->seen_idx++;

      // Skip our own published events
      for (int i = 0; i < 64; i++) {
        if (strcmp(ctx->own_ids[i], event_id) == 0) return;
      }
    }

    // For wave1: content is plaintext JSON (no NIP-04 encryption yet)
    // TODO: Add NIP-04 decryption
    char content[NOSTR_MAX_MSG];
    if (content_len >= sizeof(content)) return;

    // Unescape JSON string
    size_t j = 0;
    for (size_t i = 0; i < content_len && j < sizeof(content) - 1; i++) {
      if (content_key[i] == '\\' && i + 1 < content_len) {
        i++;
        if (content_key[i] == 'n') content[j++] = '\n';
        else if (content_key[i] == 'r') content[j++] = '\r';
        else if (content_key[i] == 't') content[j++] = '\t';
        else content[j++] = content_key[i];
      } else {
        content[j++] = content_key[i];
      }
    }
    content[j] = '\0';

    // Parse signal message
    nostr_signal_t msg;
    if (parse_signal_message(content, j, &msg) == 0) {
      if (ctx->on_signal) {
        ctx->on_signal(&msg, ctx->userdata);
      }
    }
  }

  if (ev == MG_EV_ERROR) {
    fprintf(stderr, "  nostr: relay error: %s\n", (char *)ev_data);
  }

  if (ev == MG_EV_CLOSE) {
    fprintf(stderr, "  nostr: relay disconnected\n");
    // Clear this relay from the list
    for (int i = 0; i < ctx->relay_count; i++) {
      if (ctx->relays[i] == c) {
        ctx->relays[i] = NULL;
        break;
      }
    }
  }
}

// ── Public API ──

void nostr_connect(nostr_ctx_t *ctx) {
  for (int i = 0; i < ctx->relay_count; i++) {
    fprintf(stderr, "  nostr: connecting to %s\n", ctx->relay_urls[i]);
    ctx->relays[i] = mg_ws_connect(ctx->mgr, ctx->relay_urls[i],
                                    relay_handler, ctx, NULL);
  }
}

void nostr_publish(nostr_ctx_t *ctx, nostr_signal_t *msg) {
  // Build the signal message JSON
  char content[NOSTR_MAX_MSG];
  int content_len = 0;

  switch (msg->type) {
  case NOSTR_MSG_OFFER: {
    char escaped_sdp[NOSTR_MAX_MSG];
    escape_json_string(msg->sdp, escaped_sdp, sizeof(escaped_sdp));
    content_len = snprintf(content, sizeof(content),
                           "{\"type\":\"offer\",\"sdp\":\"%s\"}", escaped_sdp);
    break;
  }
  case NOSTR_MSG_ANSWER: {
    char escaped_sdp[NOSTR_MAX_MSG];
    escape_json_string(msg->sdp, escaped_sdp, sizeof(escaped_sdp));
    content_len = snprintf(content, sizeof(content),
                           "{\"type\":\"answer\",\"sdp\":\"%s\"}", escaped_sdp);
    break;
  }
  case NOSTR_MSG_CANDIDATE:
    content_len = snprintf(content, sizeof(content),
                           "{\"type\":\"candidate\",\"candidate\":\"%s\","
                           "\"sdpMid\":\"%s\",\"sdpMLineIndex\":%d}",
                           msg->candidate, msg->sdp_mid, msg->sdp_mline_index);
    break;
  }

  if (content_len <= 0) return;

  // Escape content for JSON string context (used in both ID computation and wire format)
  char escaped_content[NOSTR_MAX_MSG * 2];
  escape_json_string(content, escaped_content, sizeof(escaped_content));

  // Build Nostr event
  int64_t created_at = (int64_t)time(NULL);
  char tags_json[256];
  snprintf(tags_json, sizeof(tags_json), "[[\"d\",\"%s\"]]", ctx->topic);

  // Compute event ID = SHA-256([0,"<pubkey>",<created_at>,<kind>,<tags>,"<escaped_content>"])
  unsigned char event_id[32];
  if (nostr_event_id(ctx->pubkey_hex, created_at, 20100, tags_json,
                     escaped_content, event_id) < 0) {
    return;
  }
  char event_id_hex[65];
  to_hex(event_id, 32, event_id_hex);

  // Track as our own (so we skip it when received back via subscription)
  strncpy(ctx->own_ids[ctx->own_idx % 64], event_id_hex, 64);
  ctx->own_idx++;

  // Sign
  unsigned char sig[64];
  if (nostr_sign_event(ctx->privkey, event_id, sig) < 0) {
    fprintf(stderr, "  nostr: signing failed\n");
    return;
  }
  char sig_hex[129];
  to_hex(sig, 64, sig_hex);

  // Build the EVENT message
  char event_json[NOSTR_MAX_MSG * 3];
  int n = snprintf(event_json, sizeof(event_json),
                   "[\"EVENT\",{\"id\":\"%s\",\"pubkey\":\"%s\","
                   "\"created_at\":%lld,\"kind\":20100,"
                   "\"tags\":[[\"d\",\"%s\"]],"
                   "\"content\":\"%s\",\"sig\":\"%s\"}]",
                   event_id_hex, ctx->pubkey_hex,
                   (long long)created_at, ctx->topic,
                   escaped_content, sig_hex);

  if (n <= 0 || (size_t)n >= sizeof(event_json)) return;

  // Send to all connected relays
  for (int i = 0; i < ctx->relay_count; i++) {
    if (ctx->relays[i]) {
      mg_ws_send(ctx->relays[i], event_json, (size_t)n, WEBSOCKET_OP_TEXT);
    }
  }
}

void nostr_set_handler(nostr_ctx_t *ctx, nostr_on_signal_fn fn, void *userdata) {
  ctx->on_signal = fn;
  ctx->userdata = userdata;
}

void nostr_close(nostr_ctx_t *ctx) {
  for (int i = 0; i < ctx->relay_count; i++) {
    if (ctx->relays[i]) {
      ctx->relays[i]->is_draining = 1;
      ctx->relays[i] = NULL;
    }
  }
}
