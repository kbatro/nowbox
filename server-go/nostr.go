package main

import (
	"context"
	"encoding/json"
	"fmt"
	"sync"
	"time"

	"github.com/nbd-wtf/go-nostr"
	"github.com/nbd-wtf/go-nostr/nip04"
)

type SignalMessage struct {
	Type          string  `json:"type"`
	SDP           string  `json:"sdp,omitempty"`
	Candidate     string  `json:"candidate,omitempty"`
	SDPMid        string  `json:"sdpMid,omitempty"`
	SDPMLineIndex *uint16 `json:"sdpMLineIndex,omitempty"`
}

type NostrSignaler struct {
	privateKey string
	publicKey  string
	topic      string
	relayURLs  []string
	relays     []*nostr.Relay
	seen       map[string]bool
	mu         sync.Mutex
}

func NewNostrSignaler(secret string) *NostrSignaler {
	priv, pub := DeriveNostrKeys(secret)
	topic := DeriveTopic(secret)
	return &NostrSignaler{
		privateKey: priv,
		publicKey:  pub,
		topic:      topic,
		relayURLs: []string{
			"wss://relay.damus.io",
			"wss://nos.lol",
			"wss://relay.nostr.band",
		},
		seen: make(map[string]bool),
	}
}

func (ns *NostrSignaler) Connect(ctx context.Context) error {
	for _, url := range ns.relayURLs {
		relay, err := nostr.RelayConnect(ctx, url)
		if err != nil {
			fmt.Printf("  warning: failed to connect to %s: %v\n", url, err)
			continue
		}
		ns.relays = append(ns.relays, relay)
		fmt.Printf("  connected to %s\n", url)
	}
	if len(ns.relays) == 0 {
		return fmt.Errorf("failed to connect to any relay")
	}
	return nil
}

func (ns *NostrSignaler) Publish(ctx context.Context, msg SignalMessage) error {
	payload, err := json.Marshal(msg)
	if err != nil {
		return err
	}

	encrypted, err := nip04.Encrypt(string(payload), ns.sharedSecret())
	if err != nil {
		return fmt.Errorf("nip04 encrypt: %w", err)
	}

	event := nostr.Event{
		Kind:      20100,
		CreatedAt: nostr.Timestamp(time.Now().Unix()),
		Tags:      nostr.Tags{{"d", ns.topic}},
		Content:   encrypted,
		PubKey:    ns.publicKey,
	}
	event.Sign(ns.privateKey)

	for _, relay := range ns.relays {
		err := relay.Publish(ctx, event)
		if err != nil {
			fmt.Printf("  warning: publish to %s failed: %v\n", relay.URL, err)
		}
	}
	return nil
}

func (ns *NostrSignaler) Subscribe(ctx context.Context) (<-chan SignalMessage, error) {
	ch := make(chan SignalMessage, 32)

	filters := nostr.Filters{{
		Kinds: []int{20100},
		Tags:  nostr.TagMap{"d": []string{ns.topic}},
		Since: func() *nostr.Timestamp { t := nostr.Timestamp(time.Now().Unix() - 60); return &t }(),
	}}

	for _, relay := range ns.relays {
		sub, err := relay.Subscribe(ctx, filters)
		if err != nil {
			fmt.Printf("  warning: subscribe to %s failed: %v\n", relay.URL, err)
			continue
		}
		go func(s *nostr.Subscription) {
			for event := range s.Events {
				ns.mu.Lock()
				if ns.seen[event.ID] {
					ns.mu.Unlock()
					continue
				}
				ns.seen[event.ID] = true
				ns.mu.Unlock()

				// Skip our own events
				if event.PubKey == ns.publicKey {
					// Both sides use same key, so filter by checking
					// if we published this event ID ourselves.
					// For now, we handle this at the caller level.
				}

				decrypted, err := nip04.Decrypt(event.Content, ns.sharedSecret())
				if err != nil {
					fmt.Printf("  warning: decrypt failed: %v\n", err)
					continue
				}

				var msg SignalMessage
				if err := json.Unmarshal([]byte(decrypted), &msg); err != nil {
					fmt.Printf("  warning: unmarshal failed: %v\n", err)
					continue
				}

				ch <- msg
			}
		}(sub)
	}

	return ch, nil
}

func (ns *NostrSignaler) Close() {
	for _, relay := range ns.relays {
		relay.Close()
	}
}

func (ns *NostrSignaler) sharedSecret() string {
	// NIP-04 shared secret: ECDH of private key with public key (encrypting to self)
	ss, _ := nip04.ComputeSharedSecret(ns.publicKey, ns.privateKey)
	return ss
}
