#ifndef NOSTR_H
#define NOSTR_H

#include "mongoose.h"
#include <stdint.h>

// Nostr signaling for WebRTC rendezvous
// Uses Mongoose WebSocket client to connect to public relays
// Uses secp256k1 for Schnorr signing and ECDH (NIP-04)

#define NOSTR_MAX_RELAYS 5
#define NOSTR_MAX_MSG 4096

typedef enum {
  NOSTR_MSG_OFFER,
  NOSTR_MSG_ANSWER,
  NOSTR_MSG_CANDIDATE,
} nostr_msg_type_t;

typedef struct {
  nostr_msg_type_t type;
  char sdp[NOSTR_MAX_MSG];
  char candidate[512];
  char sdp_mid[32];
  int sdp_mline_index;
} nostr_signal_t;

// Callback when a signaling message arrives from a peer
typedef void (*nostr_on_signal_fn)(nostr_signal_t *msg, void *userdata);

typedef struct {
  // Keys derived from secret
  unsigned char privkey[32];
  unsigned char pubkey[32];
  char privkey_hex[65];
  char pubkey_hex[65];
  char topic[65];

  // NIP-04 shared secret (ECDH of privkey with pubkey)
  unsigned char shared_secret[32];

  // Relay connections (Mongoose WebSocket clients)
  struct mg_connection *relays[NOSTR_MAX_RELAYS];
  const char *relay_urls[NOSTR_MAX_RELAYS];
  int relay_count;

  // Event dedup
  char seen_ids[64][65]; // last 64 event IDs
  int seen_idx;

  // Our own published event IDs (to skip when received back)
  char own_ids[64][65];
  int own_idx;

  // Callback
  nostr_on_signal_fn on_signal;
  void *userdata;

  // Mongoose manager (borrowed, not owned)
  struct mg_mgr *mgr;
} nostr_ctx_t;

// Initialize from a shared secret string
int nostr_init(nostr_ctx_t *ctx, struct mg_mgr *mgr, const char *secret);

// Connect to relays (call after mg_mgr_init, connections happen async)
void nostr_connect(nostr_ctx_t *ctx);

// Publish a signaling message to all connected relays
void nostr_publish(nostr_ctx_t *ctx, nostr_signal_t *msg);

// Set callback for incoming signaling messages
void nostr_set_handler(nostr_ctx_t *ctx, nostr_on_signal_fn fn, void *userdata);

// Cleanup
void nostr_close(nostr_ctx_t *ctx);

#endif
