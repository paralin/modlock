package sandbox

import (
	"context"
	"os"
	"os/exec"
	"path/filepath"
	"slices"
	"sync"
	"testing"
	"time"

	"github.com/paralin/modlock/src/host_app/control"
)

// TestSandbox runs a Go mod that answers /hello: the stand-in player joins,
// the command reaches the mod and its chat comes back as a message, an
// unknown command and a press in a missing mod go unhandled, a layout press
// the game could not carry is refused, and a reload restarts the mod.
func TestSandbox(t *testing.T) {
	// Build the mod.
	dist := t.TempDir()
	cmd := exec.Command("go", "build", "-buildmode=c-shared", "-o", filepath.Join(dist, "mod.wasm"), "../check/testdata/hello")
	cmd.Env = append(os.Environ(), "GOOS=wasip1", "GOARCH=wasm")
	if out, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("build the mod: %v\n%s", err, out)
	}
	manifest := `{"slug": "hello", "version": "0.1.0", "runtime": "RUNTIME_WASM", "entry": "mod.wasm"}`
	if err := os.WriteFile(filepath.Join(dist, "mod.json"), []byte(manifest), 0o644); err != nil {
		t.Fatal(err)
	}

	// Start the sandbox, keeping each event as one line.
	var mu sync.Mutex
	var events []string
	messaged := make(chan struct{}, 1)
	ctx := context.Background()
	box := Start(ctx, Config{
		Mods: []string{dist},
		Events: func(event *control.HostEvent) {
			mu.Lock()
			defer mu.Unlock()
			events = append(events, event.String())
			if event.GetMessage() != nil {
				messaged <- struct{}{}
			}
		},
	})
	defer box.Stop()

	// Send the commands, then reload.
	if handled, err := box.Command(ctx, "/hello"); err != nil || !handled {
		t.Fatalf("/hello: handled %v, %v", handled, err)
	}
	if handled, err := box.Command(ctx, "nope"); err != nil || handled {
		t.Fatalf("nope: handled %v, %v", handled, err)
	}

	// A claimed command runs with the next frame.
	select {
	case <-messaged:
	case <-time.After(10 * time.Second):
		t.Fatal("/hello did not answer within 10 s")
	}
	if pressed, err := box.Press(ctx, "missing", 0, "button", "", ""); err != nil || pressed {
		t.Fatalf("press in a missing mod: pressed %v, %v", pressed, err)
	}

	// A layout's press the game could not carry is refused before any mod.
	for _, refused := range [][2]string{
		{"buy now", ""},
		{"", "{}"},
		{"buy", string(make([]byte, 181))},
		{"buy", "\xff"},
	} {
		if pressed, err := box.Press(ctx, "hello", 0, "card", refused[0], refused[1]); err == nil || pressed {
			t.Fatalf("press %q %q: pressed %v, %v", refused[0], refused[1], pressed, err)
		}
	}
	if err := box.Reload(ctx, dist); err != nil {
		t.Fatal(err)
	}
	box.Stop()
	if err := box.Wait(); err != nil {
		t.Fatal(err)
	}

	// Check what a server would have reported.
	want := []string{
		(&control.HostEvent{Body: &control.HostEvent_Ready{Ready: &control.ServerReady{}}}).String(),
		(&control.HostEvent{Body: &control.HostEvent_Started{Started: &control.ModStarted{Mod: "hello"}}}).String(),
		(&control.HostEvent{Body: &control.HostEvent_Joined{Joined: &control.PlayerJoined{Name: "Player", SteamId: player.GetSteamId()}}}).String(),
		(&control.HostEvent{Body: &control.HostEvent_Message{Message: &control.PlayerMessage{Mod: "hello", Text: "Hello!"}}}).String(),
		(&control.HostEvent{Body: &control.HostEvent_Started{Started: &control.ModStarted{Mod: "hello", Reloaded: true}}}).String(),
	}
	if !slices.Equal(events, want) {
		t.Fatalf("got events\n%q\nwant\n%q", events, want)
	}
}
