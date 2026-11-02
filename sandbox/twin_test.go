package sandbox

import (
	"context"
	"errors"
	"os"
	"os/exec"
	"path/filepath"
	"testing"
)

// TestTwin builds one mod three ways and plays pairs of them in a Twin: two
// builds that differ only in the order of their start commands match, though
// each draws random numbers and reads the clock, and a build whose frame log
// differs diverges at the first such frame with both lines named.
func TestTwin(t *testing.T) {
	ctx := context.Background()

	// Build the mod as written, with its start commands reversed, and with
	// another dagger.
	dir := t.TempDir()
	build := func(name string, flags ...string) []byte {
		path := filepath.Join(dir, name+".wasm")
		args := append([]string{"build", "-buildmode=c-shared", "-o", path}, flags...)
		cmd := exec.Command("go", append(args, "./testdata/twin")...)
		cmd.Env = append(os.Environ(), "GOOS=wasip1", "GOARCH=wasm")
		if out, err := cmd.CombinedOutput(); err != nil {
			t.Fatalf("build %s: %v\n%s", name, err, out)
		}
		module, err := os.ReadFile(path)
		if err != nil {
			t.Fatal(err)
		}
		return module
	}
	written := build("written")
	reordered := build("reordered", "-ldflags=-X 'main.order=sv_hibernate_when_empty 0,citadel_koth_enabled 0,sv_cheats 1'")
	copied := build("copied", "-ldflags=-X main.dagger=0")

	// play runs a pair through the sandbox's session.
	play := func(reference, candidate []byte) (*Twin, error) {
		twin, err := NewTwin(ctx, reference, candidate, nil, nil)
		if err != nil {
			t.Fatal(err)
		}
		t.Cleanup(func() { _ = twin.Close(ctx) })
		return twin, twin.Play(ctx, nil, "dl_midtown", 90)
	}

	// The reordered commands compare as a set.
	if twin, err := play(written, reordered); err != nil {
		t.Fatalf("reordered start commands: %v", err)
	} else if twin.Events() != 3+90 {
		t.Fatalf("played %d events, want %d", twin.Events(), 3+90)
	}

	// The other dagger diverges at frame 30, the 33rd event.
	_, err := play(written, copied)
	var divergence *Divergence
	if !errors.As(err, &divergence) {
		t.Fatalf("other dagger: got %v, want a divergence", err)
	}
	if divergence.Event != 33 || divergence.Method != "Frame" {
		t.Fatalf("diverged at event %d (%s), want 33 (Frame): %v", divergence.Event, divergence.Method, divergence)
	}
	t.Log(divergence)
}
