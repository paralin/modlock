package main

import (
	"context"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/paralin/modlock/src/host_app/control"
	persistpb "github.com/paralin/modlock/src/wasm/persist"
)

// TestLocalPersist answers a game session's persist calls from the project's
// files for the player the host says is in each slot, even when the session
// forwards its other services, and keeps the records past the session.
func TestLocalPersist(t *testing.T) {
	// A project declares a record type and another service.
	dir := t.TempDir()
	build := filepath.Join(dir, "build")
	manifest := `{"slug":"prophunt","services":[{"name":"persist"},{"name":"example.drills.v1"}],` +
		`"persist":[{"name":"stats","writers":"WRITERS_SERVERS","fields":{"roundsWon":{"kind":"KIND_COUNT"}}}]}`
	for _, file := range []string{filepath.Join(dir, "mod.json"), filepath.Join(build, "mod.json")} {
		if err := os.MkdirAll(filepath.Dir(file), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(file, []byte(manifest), 0o644); err != nil {
			t.Fatal(err)
		}
	}

	// The fixtures forward persist to this computer, the rest to the origin.
	records, err := newLocalPersist(&printer{brief: true}, []string{build})
	if err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(t.TempDir(), "fixtures.pb")
	if _, err := writeFixtures(&printer{brief: true}, []string{build}, path, "http://127.0.0.1:9", records); err != nil {
		t.Fatal(err)
	}
	data, _ := os.ReadFile(path)
	fixtures := &control.Fixtures{}
	if err := fixtures.UnmarshalVT(data); err != nil {
		t.Fatal(err)
	}
	var targets []string
	for _, forward := range fixtures.GetForwards() {
		targets = append(targets, forward.GetService()+" "+forward.GetTarget())
	}
	if strings.Join(targets, ", ") != "persist this computer, example.drills.v1 http://127.0.0.1:9" {
		t.Fatalf("forwards %v", targets)
	}

	// A player joins slot 2 and a bot slot 3.
	events := records.follow(func(*control.HostEvent) {})
	events(&control.HostEvent{Body: &control.HostEvent_Joined{Joined: &control.PlayerJoined{Slot: 2, SteamId: 76561197960265729}}})
	events(&control.HostEvent{Body: &control.HostEvent_Joined{Joined: &control.PlayerJoined{Slot: 3}}})
	var passed []string
	answer := records.answer(func(_ context.Context, call *control.ServiceCall) ([]byte, error) {
		passed = append(passed, call.GetService())
		return nil, nil
	})
	update := func(slot int32) error {
		payload, _ := (&persistpb.UpdateRequest{Player: slot, Type: "stats", Ops: []*persistpb.Op{{Field: "roundsWon", Op: &persistpb.Op_Add{Add: 1}}}}).MarshalVT()
		_, err := answer(t.Context(), &control.ServiceCall{Mod: "prophunt", Service: "persist", Method: "Update", Payload: payload})
		return err
	}

	// The player's update lands in the project's file; the bot's is refused.
	if err := update(2); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(filepath.Join(dir, ".modlock", "persist", "76561197960265729.json")); err != nil {
		t.Fatal(err)
	}
	if err := update(3); err == nil || !strings.Contains(err.Error(), "no player is in slot 3") {
		t.Fatalf("bot: %v", err)
	}

	// After the player leaves, the slot is empty again.
	events(&control.HostEvent{Body: &control.HostEvent_Left{Left: &control.PlayerLeft{Slot: 2}}})
	if err := update(2); err == nil {
		t.Fatal("a left player's slot still reaches their record")
	}

	// Other services go to the forwarder.
	if _, err := answer(t.Context(), &control.ServiceCall{Mod: "prophunt", Service: "example.drills.v1"}); err != nil || len(passed) != 1 {
		t.Fatalf("forwarded %v %v", passed, err)
	}
}
