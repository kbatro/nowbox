#include "webrtc.h"

#include <rtc/rtc.h>
#include <stdio.h>
#include <string.h>

// ── libdatachannel callbacks ──

static void on_local_description(int pc, const char *sdp, const char *type, void *ptr) {
  (void)pc;
  (void)sdp;
  webrtc_ctx_t *ctx = (webrtc_ctx_t *)ptr;
  fprintf(stderr, "  webrtc: local description (%s) ready\n", type);
  // Don't publish yet — wait for gathering to complete
}

static void on_local_candidate(int pc, const char *cand, const char *mid, void *ptr) {
  (void)pc;
  (void)mid;
  (void)ptr;
  if (!cand || strlen(cand) == 0) return;
  fprintf(stderr, "  webrtc: local candidate: %.60s...\n", cand);
  // Don't publish individually — they'll be in the full SDP after gathering
}

static void on_state_change(int pc, rtcState state, void *ptr) {
  (void)pc;
  (void)ptr;
  const char *names[] = {"new", "connecting", "connected", "disconnected", "failed", "closed"};
  fprintf(stderr, "  webrtc: state -> %s\n", names[state]);
}

static void on_gathering_state(int pc, rtcGatheringState state, void *ptr) {
  webrtc_ctx_t *ctx = (webrtc_ctx_t *)ptr;
  const char *names[] = {"new", "inprogress", "complete"};
  fprintf(stderr, "  webrtc: gathering -> %s\n", names[state]);

  if (state == RTC_GATHERING_COMPLETE) {
    // Now get the full SDP with all candidates included
    char sdp[NOSTR_MAX_MSG];
    int len = rtcGetLocalDescription(pc, sdp, sizeof(sdp));
    if (len <= 0) return;
    sdp[len] = '\0';

    fprintf(stderr, "  webrtc: publishing full offer (%d bytes) via nostr\n", len);

    nostr_signal_t sig;
    memset(&sig, 0, sizeof(sig));
    sig.type = NOSTR_MSG_OFFER;
    memcpy(sig.sdp, sdp, (size_t)len + 1);

    nostr_publish(ctx->nostr, &sig);
    ctx->offer_published = 1;
  }
}

static void on_dc_open(int dc, void *ptr) {
  (void)dc;
  webrtc_ctx_t *ctx = (webrtc_ctx_t *)ptr;
  ctx->connected = 1;
  fprintf(stderr, "\n  ● DataChannel open — peer connected!\n\n");
  if (ctx->on_open) ctx->on_open(ctx->userdata);
}

static void on_dc_closed(int dc, void *ptr) {
  (void)dc;
  webrtc_ctx_t *ctx = (webrtc_ctx_t *)ptr;
  ctx->connected = 0;
  fprintf(stderr, "  webrtc: DataChannel closed\n");
  if (ctx->on_close) ctx->on_close(ctx->userdata);
}

static void on_dc_error(int dc, const char *error, void *ptr) {
  (void)dc;
  (void)ptr;
  fprintf(stderr, "  webrtc: DataChannel error: %s\n", error);
}

static void on_dc_message(int dc, const char *message, int size, void *ptr) {
  (void)dc;
  webrtc_ctx_t *ctx = (webrtc_ctx_t *)ptr;
  if (ctx->on_message) {
    ctx->on_message(message, size, ctx->userdata);
  }
}

// Called when the remote peer creates a DataChannel (we're the answerer)
static void on_data_channel(int pc, int dc, void *ptr) {
  (void)pc;
  webrtc_ctx_t *ctx = (webrtc_ctx_t *)ptr;
  ctx->dc = dc;

  rtcSetOpenCallback(dc, on_dc_open);
  rtcSetClosedCallback(dc, on_dc_closed);
  rtcSetErrorCallback(dc, on_dc_error);
  rtcSetMessageCallback(dc, on_dc_message);
  rtcSetUserPointer(dc, ctx);

  fprintf(stderr, "  webrtc: remote DataChannel created\n");
}

// ── Public API ──

int webrtc_init(webrtc_ctx_t *ctx, nostr_ctx_t *nostr) {
  memset(ctx, 0, sizeof(*ctx));
  ctx->nostr = nostr;
  ctx->pc = -1;
  ctx->dc = -1;

  // Initialize libdatachannel logging
  rtcInitLogger(RTC_LOG_WARNING, NULL);

  // Configure ICE servers
  const char *stun[] = {"stun:stun.l.google.com:19302"};

  rtcConfiguration config;
  memset(&config, 0, sizeof(config));
  config.iceServers = stun;
  config.iceServersCount = 1;

  // Create peer connection
  ctx->pc = rtcCreatePeerConnection(&config);
  if (ctx->pc < 0) {
    fprintf(stderr, "  webrtc: failed to create peer connection\n");
    return -1;
  }

  rtcSetUserPointer(ctx->pc, ctx);

  // Set callbacks
  rtcSetLocalDescriptionCallback(ctx->pc, on_local_description);
  rtcSetLocalCandidateCallback(ctx->pc, on_local_candidate);
  rtcSetStateChangeCallback(ctx->pc, on_state_change);
  rtcSetGatheringStateChangeCallback(ctx->pc, on_gathering_state);
  rtcSetDataChannelCallback(ctx->pc, on_data_channel);

  // Create DataChannel (we are the offerer)
  ctx->dc = rtcCreateDataChannel(ctx->pc, "echo");
  if (ctx->dc < 0) {
    fprintf(stderr, "  webrtc: failed to create data channel\n");
    return -1;
  }

  rtcSetUserPointer(ctx->dc, ctx);
  rtcSetOpenCallback(ctx->dc, on_dc_open);
  rtcSetClosedCallback(ctx->dc, on_dc_closed);
  rtcSetErrorCallback(ctx->dc, on_dc_error);
  rtcSetMessageCallback(ctx->dc, on_dc_message);

  // Setting local description triggers offer generation
  // The on_local_description callback will publish it via Nostr
  rtcSetLocalDescription(ctx->pc, "offer");

  fprintf(stderr, "  webrtc: peer connection created, generating offer...\n");
  return 0;
}

void webrtc_on_signal(webrtc_ctx_t *ctx, nostr_signal_t *msg) {
  switch (msg->type) {
  case NOSTR_MSG_OFFER:
    // We are the offerer, ignore offers (they're our own)
    break;

  case NOSTR_MSG_ANSWER:
    fprintf(stderr, "  webrtc: received answer from peer (%zu bytes)\n", strlen(msg->sdp));
    fprintf(stderr, "  webrtc: answer SDP first 200 chars: %.200s\n", msg->sdp);
    rtcSetRemoteDescription(ctx->pc, msg->sdp, "answer");
    ctx->has_remote_desc = 1;

    // Flush queued candidates
    for (int i = 0; i < ctx->queued_count; i++) {
      fprintf(stderr, "  webrtc: adding queued candidate\n");
      rtcAddRemoteCandidate(ctx->pc, ctx->queued_candidates[i].candidate,
                            strlen(ctx->queued_candidates[i].sdp_mid) > 0
                              ? ctx->queued_candidates[i].sdp_mid : "0");
    }
    ctx->queued_count = 0;
    break;

  case NOSTR_MSG_CANDIDATE:
    if (strlen(msg->candidate) == 0) break;

    if (!ctx->has_remote_desc) {
      // Queue until we have the remote description
      if (ctx->queued_count < 32) {
        ctx->queued_candidates[ctx->queued_count++] = *msg;
      }
    } else {
      rtcAddRemoteCandidate(ctx->pc, msg->candidate,
                            strlen(msg->sdp_mid) > 0 ? msg->sdp_mid : "0");
    }
    break;
  }
}

int webrtc_send(webrtc_ctx_t *ctx, const char *data, int len) {
  if (ctx->dc < 0 || !ctx->connected) return -1;
  return rtcSendMessage(ctx->dc, data, len);
}

void webrtc_republish_offer(webrtc_ctx_t *ctx) {
  if (!ctx->offer_published || ctx->pc < 0) return;

  char sdp[NOSTR_MAX_MSG];
  int len = rtcGetLocalDescription(ctx->pc, sdp, sizeof(sdp));
  if (len <= 0) return;

  nostr_signal_t sig;
  memset(&sig, 0, sizeof(sig));
  sig.type = NOSTR_MSG_OFFER;
  memcpy(sig.sdp, sdp, (size_t)len);
  sig.sdp[len] = '\0';

  nostr_publish(ctx->nostr, &sig);
}

void webrtc_set_callbacks(webrtc_ctx_t *ctx,
                          webrtc_on_open_fn on_open,
                          webrtc_on_message_fn on_message,
                          webrtc_on_close_fn on_close,
                          void *userdata) {
  ctx->on_open = on_open;
  ctx->on_message = on_message;
  ctx->on_close = on_close;
  ctx->userdata = userdata;
}

void webrtc_close(webrtc_ctx_t *ctx) {
  if (ctx->dc >= 0) rtcDeleteDataChannel(ctx->dc);
  if (ctx->pc >= 0) {
    rtcClosePeerConnection(ctx->pc);
    rtcDeletePeerConnection(ctx->pc);
  }
  ctx->dc = -1;
  ctx->pc = -1;
}
