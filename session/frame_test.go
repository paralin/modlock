package session

import (
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/paralin/modlock/src/host_app/control"
)

// TestFrame frames each human who joins, never a bot, and redraws everyone's
// frame with the new count when a mod reloads.
func TestFrame(t *testing.T) {
	// A built mod and its session's frame.
	dir := t.TempDir()
	manifest := `{"slug": "blind-draft-duel", "name": "Blind Draft Duel", "version": "0.1.1"}`
	if err := os.WriteFile(filepath.Join(dir, "mod.json"), []byte(manifest), 0o644); err != nil {
		t.Fatal(err)
	}
	frame := NewFrame("v0.1.67", []string{dir})
	joined := func(slot int32, steamID uint64) *control.HostEvent {
		return &control.HostEvent{Body: &control.HostEvent_Joined{Joined: &control.PlayerJoined{Slot: slot, SteamId: steamID}}}
	}
	line := func(events []*control.HostEvent) string {
		for _, node := range events[0].GetUi().GetChange().GetSet() {
			if node.GetId() == "line" {
				return node.GetText()
			}
		}
		return ""
	}

	// A human gets the frame; a bot does not.
	events := frame.Follow(joined(1, 76561198000000001))
	if len(events) != 1 || events[0].GetUi().GetMod() != FrameMod || events[0].GetUi().GetSlot() != 1 {
		t.Fatal("join", events)
	}
	if got := line(events); got != "Blind Draft Duel 0.1.1 · reload 0 · modlock v0.1.67" {
		t.Fatal("line", got)
	}
	if events := frame.Follow(joined(2, 0)); len(events) != 0 {
		t.Fatal("bot framed", events)
	}

	// A reload redraws every human's frame with the count.
	frame.Follow(joined(3, 76561198000000003))
	reload := &control.HostEvent{Body: &control.HostEvent_Started{Started: &control.ModStarted{Mod: "blind-draft-duel", Reloaded: true}}}
	events = frame.Follow(reload)
	if len(events) != 2 || !strings.Contains(line(events), "reload 1") {
		t.Fatal("reload", events)
	}
}
