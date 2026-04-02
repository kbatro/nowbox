#ifndef WEBRTC_H
#define WEBRTC_H

#include "nostr.h"

// WebRTC peer connection using libdatachannel
// Publishes offer + ICE candidates via Nostr
// Receives answer + ICE candidates via Nostr
// Opens a DataChannel for terminal I/O

typedef void (*webrtc_on_open_fn)(void *userdata);
typedef void (*webrtc_on_message_fn)(const char *data, int len, void *userdata);
typedef void (*webrtc_on_close_fn)(void *userdata);

typedef struct {
  int pc;  // peer connection id from libdatachannel
  int dc;  // data channel id from libdatachannel
  nostr_ctx_t *nostr;

  webrtc_on_open_fn on_open;
  webrtc_on_message_fn on_message;
  webrtc_on_close_fn on_close;
  void *userdata;

  int connected;
  int offer_published;
  int has_remote_desc;

  // Queued candidates (received before remote description)
  nostr_signal_t queued_candidates[32];
  int queued_count;
} webrtc_ctx_t;

// Create peer connection and data channel, publish offer via Nostr
int webrtc_init(webrtc_ctx_t *ctx, nostr_ctx_t *nostr);

// Handle incoming Nostr signaling message (call from nostr on_signal callback)
void webrtc_on_signal(webrtc_ctx_t *ctx, nostr_signal_t *msg);

// Send data over the DataChannel
int webrtc_send(webrtc_ctx_t *ctx, const char *data, int len);

// Republish the offer (for late-joining clients)
void webrtc_republish_offer(webrtc_ctx_t *ctx);

// Set callbacks
void webrtc_set_callbacks(webrtc_ctx_t *ctx,
                          webrtc_on_open_fn on_open,
                          webrtc_on_message_fn on_message,
                          webrtc_on_close_fn on_close,
                          void *userdata);

// Cleanup
void webrtc_close(webrtc_ctx_t *ctx);

#endif
