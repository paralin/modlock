package sandbox

import (
	"bytes"
	"context"
	"encoding/binary"
	"errors"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/paralin/modlock/src/wasm"
	"github.com/paralin/modlock/src/wasm/entities"
	"github.com/paralin/modlock/src/wasm/trace"
)

// TestTwin builds one mod three ways and plays pairs of them in a Twin: two
// builds that differ only in the order of their start commands match, though
// each draws random numbers and reads the clock, and a build whose frame log
// differs diverges at the first such frame with both lines named. A trace the
// host recorded replays the same way, its replies reaching both builds.
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

	// Record a trace as the host writes one: Start, then 60 frames, where the
	// host names entity 7 on each thirtieth, cut short inside a record.
	var traced bytes.Buffer
	write := func(record *trace.Record) {
		data, err := record.MarshalVT()
		if err != nil {
			t.Fatal(err)
		}
		traced.Write(binary.AppendUvarint(nil, uint64(len(data))))
		traced.Write(data)
	}
	event := func(method string, request message, at time.Time) {
		data, err := request.MarshalVT()
		if err != nil {
			t.Fatal(err)
		}
		write(&trace.Record{Entry: &trace.Record_Event{Event: &trace.Event{
			Call: &wasm.Call{Method: method, Request: data}, UnixNanos: at.UnixNano(),
		}}})
	}
	nameRequest, _ := (&entities.EntityRequest{Entity: 7}).MarshalVT()
	nameReply, _ := (&entities.EntityNameResponse{Name: "sleep_dagger"}).MarshalVT()
	start := time.Date(2026, 3, 1, 12, 0, 0, 0, time.UTC)
	event("Start", &wasm.StartEvent{}, start)
	for tick := uint64(1); tick <= 60; tick++ {
		event("Frame", &wasm.FrameEvent{Tick: tick}, start.Add(time.Duration(tick)*frameInterval))
		if tick%30 == 0 {
			write(&trace.Record{Entry: &trace.Record_Call{Call: &trace.HostCall{
				Call:  &wasm.Call{Method: "EntityName", Request: nameRequest},
				Reply: &wasm.Reply{Response: nameReply},
			}}})
		}
	}
	traced.Write([]byte{40, 1, 2})

	// The reordered pair replays all 61 events.
	replay := func(reference, candidate []byte) (*Twin, error) {
		twin, err := NewTwin(ctx, reference, candidate, nil, nil)
		if err != nil {
			t.Fatal(err)
		}
		t.Cleanup(func() { _ = twin.Close(ctx) })
		return twin, twin.Replay(ctx, bytes.NewReader(traced.Bytes()), nil)
	}
	if twin, err := replay(written, reordered); err != nil {
		t.Fatalf("replay reordered start commands: %v", err)
	} else if twin.Events() != 61 {
		t.Fatalf("replayed %d events, want 61", twin.Events())
	}

	// The other dagger diverges at frame 30, both builds holding the name the
	// trace gave.
	_, err = replay(written, copied)
	if !errors.As(err, &divergence) || divergence.Event != 31 {
		t.Fatalf("replay other dagger: got %v, want a divergence at event 31", err)
	}
	if !strings.Contains(divergence.Reference, "sleep_dagger") || !strings.Contains(divergence.Candidate, "sleep_dagger") {
		t.Fatalf("replay other dagger: the trace's reply is missing from %v", divergence)
	}
	t.Log(divergence)
}
