package main

import (
	"context"
	"flag"
	"fmt"
	"os"
	"os/signal"
	"time"
)

func main() {
	secret := flag.String("secret", "", "shared secret for this session (required)")
	flag.Parse()

	if *secret == "" {
		fmt.Fprintln(os.Stderr, "usage: nowbox-server --secret <secret>")
		os.Exit(1)
	}

	fmt.Println("nowbox-server wave1")
	fmt.Printf("  secret: %s\n", *secret)

	ctx, cancel := signal.NotifyContext(context.Background(), os.Interrupt)
	defer cancel()

	// Derive keys and connect to Nostr
	fmt.Println("\nconnecting to nostr relays...")
	signaler := NewNostrSignaler(*secret)
	if err := signaler.Connect(ctx); err != nil {
		fmt.Fprintf(os.Stderr, "nostr: %v\n", err)
		os.Exit(1)
	}
	defer signaler.Close()

	fmt.Printf("  topic: %s\n", signaler.topic[:16]+"...")
	fmt.Printf("  pubkey: %s\n", signaler.publicKey[:16]+"...")

	// Create WebRTC peer and start signaling
	fmt.Println("\ncreating webrtc peer...")
	pm, err := NewPeerManager(signaler)
	if err != nil {
		fmt.Fprintf(os.Stderr, "webrtc: %v\n", err)
		os.Exit(1)
	}

	if err := pm.Start(ctx); err != nil {
		fmt.Fprintf(os.Stderr, "start: %v\n", err)
		os.Exit(1)
	}

	fmt.Println("\nwaiting for peer...")
	fmt.Println("  (re-publishing offer every 5s)")

	// Re-publish offer periodically so late-joining clients can find us
	ticker := time.NewTicker(5 * time.Second)
	defer ticker.Stop()

	go func() {
		for {
			select {
			case <-ctx.Done():
				return
			case <-ticker.C:
				pm.RepublishOffer(ctx)
			}
		}
	}()

	<-ctx.Done()
	fmt.Println("\nshutting down")
}
