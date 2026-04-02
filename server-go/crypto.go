package main

import (
	"crypto/sha256"
	"encoding/hex"

	"github.com/nbd-wtf/go-nostr"
)

func DeriveNostrKeys(secret string) (privKeyHex string, pubKeyHex string) {
	hash := sha256.Sum256([]byte(secret))
	privKeyHex = hex.EncodeToString(hash[:])
	pubKeyHex, _ = nostr.GetPublicKey(privKeyHex)
	return
}

func DeriveTopic(secret string) string {
	hash := sha256.Sum256([]byte("nowbox-topic:" + secret))
	return hex.EncodeToString(hash[:])
}
