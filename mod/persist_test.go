package mod

import (
	"errors"
	"testing"
	"time"

	persistpb "github.com/paralin/modlock/src/wasm/persist"
)

// TestPersistRefusesLeftPlayer refuses a persist call for a Player kept
// after its player left, before anything is sent, once another player holds
// the slot, and sends a live Player's connection generation so the host can
// check it too.
func TestPersistRefusesLeftPlayer(t *testing.T) {
	// Keep the calls the persist service would start, and restore the roster.
	held := *registered
	registered.tenures, registered.generations = map[int32]uint32{}, map[int32]uint32{}
	t.Cleanup(func() { *registered = held; callPersist = CallServiceLater })
	var sent []*persistpb.UpdateRequest
	var gets []*persistpb.GetRequest
	callPersist = func(service, method string, payload []byte, _ time.Duration, _ func([]byte, error)) {
		switch method {
		case "Update":
			request := &persistpb.UpdateRequest{}
			_ = request.UnmarshalVT(payload)
			sent = append(sent, request)
		case "Get":
			request := &persistpb.GetRequest{}
			_ = request.UnmarshalVT(payload)
			gets = append(gets, request)
		}
	}
	joined := func(steamID uint64, generation uint32) {
		_ = registered.PlayersChanged(&PlayersChangedEvent{Players: []*Connection{{Player: 2, SteamId: steamID, Generation: generation}}})
	}
	won := []*PersistOp{PersistAdd("roundsWon", 1)}

	// A player in slot 2 wins; the update names their connection.
	joined(76561197960265728, 7)
	kept := PlayerOf(2)
	kept.UpdateRecord("stats", won, "round-1", nil)
	if len(sent) != 1 || sent[0].GetPlayer() != 2 || sent[0].GetGeneration() != 7 {
		t.Fatalf("live update sent %v", sent)
	}

	// They leave and another player takes slot 2. The kept Player is refused
	// at once, for an update and a read, and nothing is sent.
	joined(76561197960265729, 8)
	var refused error
	kept.UpdateRecord("stats", won, "round-2", func(_ *PersistRecord, _ bool, err error) { refused = err })
	if !errors.Is(refused, ErrPlayerLeft) {
		t.Fatalf("a kept Player's update after the slot changed hands: %v", refused)
	}
	refused = nil
	kept.GetRecord("stats", func(_ *PersistRecord, err error) { refused = err })
	if !errors.Is(refused, ErrPlayerLeft) {
		t.Fatalf("a kept Player's read after the slot changed hands: %v", refused)
	}
	if len(sent) != 1 || len(gets) != 0 {
		t.Fatalf("a kept Player's calls were sent: %v %v", sent, gets)
	}

	// The newcomer's own Player reaches the host with their generation.
	PlayerOf(2).UpdateRecord("stats", won, "round-2", nil)
	if len(sent) != 2 || sent[1].GetGeneration() != 8 {
		t.Fatalf("newcomer's update sent %v", sent)
	}

	// Once the slot is empty, no Player of it reaches the host.
	_ = registered.PlayersChanged(&PlayersChangedEvent{})
	refused = nil
	PlayerOf(2).UpdateRecord("stats", won, "round-3", func(_ *PersistRecord, _ bool, err error) { refused = err })
	if !errors.Is(refused, ErrPlayerLeft) || len(sent) != 2 {
		t.Fatalf("an empty slot's update: %v %v", refused, sent)
	}
}
