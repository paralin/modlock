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
	events(&control.HostEvent{Body: &control.HostEvent_Joined{Joined: &control.PlayerJoined{Slot: 2, SteamId: 76561197960265729, Generation: 1}}})
	events(&control.HostEvent{Body: &control.HostEvent_Joined{Joined: &control.PlayerJoined{Slot: 3, Generation: 1}}})
	var passed []string
	answer := records.answer(func(_ context.Context, call *control.ServiceCall) ([]byte, error) {
		passed = append(passed, call.GetService())
		return nil, nil
	})
	update := func(slot int32) error {
		payload, _ := (&persistpb.UpdateRequest{Player: slot, Generation: 1, Type: "stats", UpdateKey: "round-1", Ops: []*persistpb.Op{{Field: "roundsWon", Op: &persistpb.Op_Add{Add: 1}}}}).MarshalVT()
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

// TestLocalPersistDelayedRequest refuses a request made for a player who has
// since left, when it reaches the session after the next player took the
// slot, so it cannot write that player's record.
func TestLocalPersistDelayedRequest(t *testing.T) {
	// A project declares a record type.
	dir := t.TempDir()
	manifest := `{"slug":"prophunt","services":[{"name":"persist"}],` +
		`"persist":[{"name":"stats","writers":"WRITERS_SERVERS","fields":{"roundsWon":{"kind":"KIND_COUNT"}}}]}`
	build := filepath.Join(dir, "build")
	for _, file := range []string{filepath.Join(dir, "mod.json"), filepath.Join(build, "mod.json")} {
		if err := os.MkdirAll(filepath.Dir(file), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(file, []byte(manifest), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	records, err := newLocalPersist(&printer{brief: true}, []string{build})
	if err != nil {
		t.Fatal(err)
	}
	events := records.follow(func(*control.HostEvent) {})
	answer := records.answer(nil)
	update := func(generation uint32, key string) error {
		payload, _ := (&persistpb.UpdateRequest{Player: 2, Generation: generation, Type: "stats", UpdateKey: key, Ops: []*persistpb.Op{{Field: "roundsWon", Op: &persistpb.Op_Add{Add: 1}}}}).MarshalVT()
		_, err := answer(t.Context(), &control.ServiceCall{Mod: "prophunt", Service: "persist", Method: "Update", Payload: payload})
		return err
	}

	// A holds slot 2 on connection 1 and a request is made for them; A
	// leaves and B takes the slot on connection 2 before it arrives.
	const a, b = 76561197960265729, 76561197960265730
	events(&control.HostEvent{Body: &control.HostEvent_Joined{Joined: &control.PlayerJoined{Slot: 2, SteamId: a, Generation: 1}}})
	events(&control.HostEvent{Body: &control.HostEvent_Left{Left: &control.PlayerLeft{Slot: 2}}})
	events(&control.HostEvent{Body: &control.HostEvent_Joined{Joined: &control.PlayerJoined{Slot: 2, SteamId: b, Generation: 2}}})

	// The delayed request is refused and B's record is never written.
	bFile := filepath.Join(dir, ".modlock", "persist", "76561197960265730.json")
	if err := update(1, "round-1"); err == nil {
		t.Fatal("a request made for the slot's earlier player wrote the next player's record")
	}
	if _, err := os.Stat(bFile); !os.IsNotExist(err) {
		t.Fatalf("B's record was written: %v", err)
	}

	// B's own request lands.
	if err := update(2, "round-2"); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(bFile); err != nil {
		t.Fatal(err)
	}

	// A host that names no generation has every call refused.
	events(&control.HostEvent{Body: &control.HostEvent_Joined{Joined: &control.PlayerJoined{Slot: 2, SteamId: b}}})
	if err := update(2, "round-3"); err == nil || !strings.Contains(err.Error(), "left slot 2") {
		t.Fatalf("a slot with no generation: %v", err)
	}
}

// TestPersistSlugSymlinkRefused refuses a built mod whose records directory
// already exists beneath Modlock's persist directory as a link elsewhere,
// before any file is made through it.
func TestPersistSlugSymlinkRefused(t *testing.T) {
	// The persist directory holds a link named for the mod's slug.
	config := t.TempDir()
	t.Setenv("XDG_CONFIG_HOME", config)
	t.Setenv("HOME", config)
	t.Setenv("AppData", config)
	root := configPath("persist")
	if err := os.MkdirAll(root, 0o755); err != nil {
		t.Fatal(err)
	}
	outside := t.TempDir()
	if err := os.Symlink(outside, filepath.Join(root, "review-mod")); err != nil {
		t.Fatal(err)
	}
	build := filepath.Join(t.TempDir(), "built")
	if err := os.MkdirAll(build, 0o755); err != nil {
		t.Fatal(err)
	}
	manifest := `{"slug":"review-mod","services":[{"name":"persist"}],` +
		`"persist":[{"name":"stats","writers":"WRITERS_SERVERS","fields":{"roundsWon":{"kind":"KIND_COUNT"}}}]}`
	if err := os.WriteFile(filepath.Join(build, "mod.json"), []byte(manifest), 0o644); err != nil {
		t.Fatal(err)
	}

	// The directory is refused and nothing is made through the link.
	if dir, err := persistDir(build, "review-mod"); err == nil {
		t.Fatalf("records kept through a link in %q", dir)
	}
	if _, err := newLocalPersist(&printer{brief: true}, []string{build}); err == nil {
		t.Fatal("the session kept records through a link")
	}
	if entries, _ := os.ReadDir(outside); len(entries) != 0 {
		t.Fatalf("files were made outside the persist directory: %v", entries)
	}
}

// TestPersistSlugStaysInside refuses a built mod whose manifest slug would
// keep its records outside Modlock's persist directory, before any directory
// is made, on the session's path and the sandbox's.
func TestPersistSlugStaysInside(t *testing.T) {
	// A built mod outside any project names a slug that climbs out.
	config := t.TempDir()
	t.Setenv("XDG_CONFIG_HOME", config)
	t.Setenv("HOME", config)
	t.Setenv("AppData", config)
	build := filepath.Join(t.TempDir(), "built")
	if err := os.MkdirAll(build, 0o755); err != nil {
		t.Fatal(err)
	}
	manifest := `{"slug":"../../escaped","services":[{"name":"persist"}],` +
		`"persist":[{"name":"stats","writers":"WRITERS_SERVERS","fields":{"roundsWon":{"kind":"KIND_COUNT"}}}]}`
	if err := os.WriteFile(filepath.Join(build, "mod.json"), []byte(manifest), 0o644); err != nil {
		t.Fatal(err)
	}
	root := configPath("persist")
	if !strings.HasPrefix(root, config) {
		t.Fatalf("persist directory %s is outside the test's %s", root, config)
	}

	// The session refuses it and makes no directory for it.
	if _, err := newLocalPersist(&printer{brief: true}, []string{build}); err == nil || !strings.Contains(err.Error(), "slug") {
		t.Fatalf("session: %v", err)
	}
	if _, err := os.Stat(filepath.Join(root, "../../escaped")); !os.IsNotExist(err) {
		t.Fatalf("the escaped directory was made: %v", err)
	}

	// The sandbox's directory comes from the same check.
	for _, slug := range []string{"../escaped", "a/b", "", ".", "Upper"} {
		if dir, err := persistDir(build, slug); err == nil {
			t.Fatalf("slug %q kept in %q", slug, dir)
		}
	}
	if dir, err := persistDir(build, "prop-hunt"); err != nil || dir != filepath.Join(root, "prop-hunt") {
		t.Fatalf("prop-hunt: %q %v", dir, err)
	}
}
