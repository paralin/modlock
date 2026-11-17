package main

import (
	"context"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
	"time"

	modcli "github.com/paralin/modlock/cli"
	"github.com/paralin/modlock/sandbox"
	"github.com/paralin/modlock/src/host_app/control"
)

// buildProfiled builds the test mod that spends known time in its handlers,
// and returns its build directory.
func buildProfiled(t *testing.T) string {
	t.Helper()
	dist := t.TempDir()
	cmd := exec.Command("go", "build", "-buildmode=c-shared", "-o", filepath.Join(dist, "mod.wasm"), "./testdata/profiled")
	cmd.Env = append(os.Environ(), "GOOS=wasip1", "GOARCH=wasm")
	if out, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("build the mod: %v\n%s", err, out)
	}
	manifest := `{"slug": "profiled", "version": "0.1.0", "runtime": "RUNTIME_WASM", "entry": "mod.wasm"}`
	if err := os.WriteFile(filepath.Join(dist, "mod.json"), []byte(manifest), 0o644); err != nil {
		t.Fatal(err)
	}
	return dist
}

// runProfiled runs the profiled mod in the sandbox for about seconds,
// reading its timings every half second, and returns the profiler and the
// host events it passed on.
func runProfiled(t *testing.T, seconds int) (*profiler, []*control.HostEvent) {
	t.Helper()
	dist := buildProfiled(t)
	prof := newProfiler(&printer{}, []string{dist}, true)
	passed := make(chan *control.HostEvent, 1024)
	started := make(chan string, 1)
	ctx := context.Background()
	box := sandbox.Start(ctx, sandbox.Config{
		Mods: []string{dist},
		Events: prof.follow(func(event *control.HostEvent) {
			if event.GetStarted() != nil || event.GetFailed() != nil {
				select {
				case started <- event.GetFailed().GetError():
				default:
				}
			}
			passed <- event
		}),
	})
	defer box.Stop()

	// Time the mod from its start, which a loaded machine delays.
	select {
	case failure := <-started:
		if failure != "" {
			t.Fatalf("the mod failed: %s", failure)
		}
	case <-time.After(time.Minute):
		t.Fatal("the mod did not start in a minute")
	}
	for range seconds * 2 {
		time.Sleep(500 * time.Millisecond)
		prof.poll(ctx, box)
	}
	time.Sleep(500 * time.Millisecond)
	box.Stop()
	if err := box.Wait(); err != nil {
		t.Fatal(err)
	}
	close(passed)
	var events []*control.HostEvent
	for event := range passed {
		events = append(events, event)
	}
	return prof, events
}

func TestProfilerTimesEachHandler(t *testing.T) {
	prof, events := runProfiled(t, 3)

	// The profiler's own results stay out of the session's events.
	for _, event := range events {
		if called := event.GetCalled(); called != nil {
			t.Errorf("passed on the profiler's result %v", called)
		}
	}

	// The table names each handler by its line, with the frame time it spent.
	taken := prof.take(3 * time.Second)
	if len(taken) != 1 {
		t.Fatalf("took %d mods' timings, want 1", len(taken))
	}
	timings := taken[0]
	t.Log("\n" + strings.Join(timingTable(timings), "\n"))
	frames := timings.GetFrames()
	if frames.GetCalls() < 10 {
		t.Fatalf("counted %d frames in 3 s", frames.GetCalls())
	}
	if mean := frames.GetTotalMs() / float64(frames.GetCalls()); mean < 2 {
		t.Errorf("mean frame %.2f ms, want at least the 2 ms the handler spins", mean)
	}
	want := map[string]string{"OnFrame": "testdata/profiled/main.go:23", "Every": "testdata/profiled/main.go:31"}
	for _, handler := range timings.GetHandlers() {
		if at, ok := want[handler.GetKind()]; ok {
			if got := handlerName(handler); !strings.HasSuffix(got, at) {
				t.Errorf("%s named %q, want it at %s", handler.GetKind(), got, at)
			}
			delete(want, handler.GetKind())
		}
	}
	if len(want) != 0 {
		t.Errorf("no timings for %v", want)
	}
	if first := timings.GetHandlers()[0]; first.GetKind() != "OnFrame" || first.GetOver() == 0 {
		t.Errorf("the slowest handler is %s with %d runs over 4 ms, want OnFrame with its 6 ms frames", first.GetKind(), first.GetOver())
	}

	// A period's timings go once.
	if again := prof.take(3 * time.Second); len(again) != 0 {
		t.Errorf("took the timings twice: %v", again)
	}
}

func TestProfilerSkipsAModWithoutIt(t *testing.T) {
	prof := newProfiler(&printer{}, nil, false)
	passed := 0
	follow := prof.follow(func(*control.HostEvent) { passed++ })
	follow(&control.HostEvent{Body: &control.HostEvent_Started{Started: &control.ModStarted{Mod: "old"}}})
	follow(&control.HostEvent{Body: &control.HostEvent_Called{Called: &control.ModCallResult{
		Id: profileCallPrefix + "1", Mod: "old", Error: "the mod serves no service modlock.profile",
	}}})
	if passed != 1 {
		t.Fatalf("passed %d events, want only the start", passed)
	}
	calls := 0
	prof.poll(context.Background(), callCounter(func() { calls++ }))
	if calls != 0 {
		t.Fatalf("polled a mod without a profiler %d times", calls)
	}

	// A rebuild may bring one, so a restart polls again.
	follow(&control.HostEvent{Body: &control.HostEvent_Started{Started: &control.ModStarted{Mod: "old", Reloaded: true}}})
	prof.poll(context.Background(), callCounter(func() { calls++ }))
	if calls != 1 {
		t.Fatalf("polled the restarted mod %d times, want 1", calls)
	}
}

func TestTimingTableAndLocations(t *testing.T) {
	if file, line := locateSite("", "/a/b/main.go:12"); file != "/a/b/main.go" || line != 12 {
		t.Errorf("Go location split as %s:%d", file, line)
	}
	if file, line := locateSite("", "mod.js:3:9"); file != "mod.js" || line != 3 {
		t.Errorf("unmapped bundle location split as %s:%d", file, line)
	}
	if file, line := locateSite("", ""); file != "" || line != 0 {
		t.Errorf("empty location split as %q:%d", file, line)
	}
	table := timingTable(&modcli.ModTimings{
		Mod: "bounty", Seconds: 10,
		Frames:   &modcli.HandlerTiming{Kind: "frame", Calls: 600, TotalMs: 1740, MaxMs: 11.2, Over: 21},
		Handlers: []*modcli.HandlerTiming{{Kind: "onFrame", File: "src/bots.ts", Line: 40, Calls: 600, TotalMs: 1104, MaxMs: 9.7, Over: 21}},
	})
	if !strings.HasPrefix(table[0], "bounty  10 s, 600 frames: mean 2.90 ms, max 11.20 ms, 21 over 4 ms") ||
		!strings.Contains(table[2], "onFrame src/bots.ts:40") || !strings.HasSuffix(table[2], "21") {
		t.Errorf("table:\n%s", strings.Join(table, "\n"))
	}
}

// callCounter is a server whose Call runs counted.
type callCounter func()

func (c callCounter) Reload(context.Context, string) error { return nil }
func (c callCounter) Call(context.Context, *control.ModCall) error {
	c()
	return nil
}
func (c callCounter) Wait() error { return nil }
func (c callCounter) Stop()       {}
