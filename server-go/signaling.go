package main

import (
	"context"
	"encoding/json"
	"fmt"

	"github.com/pion/webrtc/v4"
)

type PeerManager struct {
	pc       *webrtc.PeerConnection
	dc       *webrtc.DataChannel
	signaler *NostrSignaler
	role     string // "offer" (server) or "answer" (client)

	// published event IDs so we can skip our own messages
	publishedOfferSDP  string
	publishedAnswerSDP string
}

func NewPeerManager(signaler *NostrSignaler) (*PeerManager, error) {
	config := webrtc.Configuration{
		ICEServers: []webrtc.ICEServer{
			{URLs: []string{"stun:stun.l.google.com:19302"}},
			{
				URLs: []string{
					"turn:freeturn.net:3478",
					"turn:freeturn.net:5349",
					"turn:freeturn.net:80?transport=tcp",
					"turn:freeturn.net:443?transport=tcp",
				},
				Username:   "free",
				Credential: "free",
			},
		},
	}

	pc, err := webrtc.NewPeerConnection(config)
	if err != nil {
		return nil, err
	}

	return &PeerManager{
		pc:       pc,
		signaler: signaler,
		role:     "offer",
	}, nil
}

func (pm *PeerManager) Start(ctx context.Context) error {
	// Create the echo DataChannel
	dc, err := pm.pc.CreateDataChannel("echo", nil)
	if err != nil {
		return err
	}
	pm.dc = dc

	dc.OnOpen(func() {
		fmt.Println("\n  ● DataChannel open — peer connected!")
	})

	dc.OnMessage(func(msg webrtc.DataChannelMessage) {
		text := string(msg.Data)
		fmt.Printf("  received: %s\n", text)
		dc.SendText(text) // echo
	})

	// Trickle ICE: publish each candidate via Nostr
	pm.pc.OnICECandidate(func(c *webrtc.ICECandidate) {
		if c == nil {
			return
		}
		candidateJSON := c.ToJSON()
		bytes, _ := json.Marshal(candidateJSON)

		var raw map[string]interface{}
		json.Unmarshal(bytes, &raw)

		idx := uint16(candidateJSON.SDPMLineIndex)
		pm.signaler.Publish(ctx, SignalMessage{
			Type:          "candidate",
			Candidate:     candidateJSON.Candidate,
			SDPMid:        *candidateJSON.SDPMid,
			SDPMLineIndex: &idx,
		})
	})

	pm.pc.OnConnectionStateChange(func(state webrtc.PeerConnectionState) {
		fmt.Printf("  connection state: %s\n", state.String())
	})

	// Create offer
	offer, err := pm.pc.CreateOffer(nil)
	if err != nil {
		return err
	}
	if err := pm.pc.SetLocalDescription(offer); err != nil {
		return err
	}
	pm.publishedOfferSDP = offer.SDP

	fmt.Println("  publishing offer via nostr...")
	pm.signaler.Publish(ctx, SignalMessage{
		Type: "offer",
		SDP:  offer.SDP,
	})

	// Listen for answer and remote candidates
	msgs, err := pm.signaler.Subscribe(ctx)
	if err != nil {
		return err
	}

	go pm.handleMessages(ctx, msgs)
	return nil
}

func (pm *PeerManager) handleMessages(ctx context.Context, msgs <-chan SignalMessage) {
	for {
		select {
		case <-ctx.Done():
			return
		case msg := <-msgs:
			switch msg.Type {
			case "offer":
				// Skip our own offer
				if msg.SDP == pm.publishedOfferSDP {
					continue
				}
				fmt.Println("  warning: received unexpected offer (we are the offerer)")

			case "answer":
				fmt.Println("  received answer via nostr")
				pm.pc.SetRemoteDescription(webrtc.SessionDescription{
					Type: webrtc.SDPTypeAnswer,
					SDP:  msg.SDP,
				})

			case "candidate":
				sdpMid := msg.SDPMid
				var idx *uint16
				if msg.SDPMLineIndex != nil {
					idx = msg.SDPMLineIndex
				}
				pm.pc.AddICECandidate(webrtc.ICECandidateInit{
					Candidate:     msg.Candidate,
					SDPMid:        &sdpMid,
					SDPMLineIndex: idx,
				})
			}
		}
	}
}

func (pm *PeerManager) RepublishOffer(ctx context.Context) {
	if pm.pc.LocalDescription() == nil {
		return
	}
	pm.signaler.Publish(ctx, SignalMessage{
		Type: "offer",
		SDP:  pm.pc.LocalDescription().SDP,
	})
}
