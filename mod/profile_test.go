package mod

import (
	"encoding/json"
	"runtime"
	"strconv"
	"strings"
	"testing"
	"time"

	wasmpb "github.com/paralin/modlock/src/wasm"
)

// profileRead calls the profiler's service with method and decodes its answer.
func profileRead(t *testing.T, method string) timings {
	t.Helper()
	reply, err := registered.Serve(&ServiceCall{Service: profileService, Method: method})
	if err != nil {
		t.Fatal(err)
	}
	var read timings
	if len(reply.GetPayload()) != 0 {
		if err := json.Unmarshal(reply.GetPayload(), &read); err != nil {
			t.Fatal(err)
		}
	}
	return read
}

// countClock counts the profiler's clock reads until the test ends.
func countClock(t *testing.T) *int {
	t.Helper()
	reads := new(int)
	held := now
	now = func() time.Time {
		*reads++
		return held()
	}
	t.Cleanup(func() { now = held })
	return reads
}

func TestProfileTimesHandlersOnlyWhenAsked(t *testing.T) {
	resetClock(t)
	t.Cleanup(func() { profiling = false })

	// Register a frame handler, a repeat and a command, each on its own line.
	frames, ticks, commands := 0, 0, 0
	_, _, line, _ := runtime.Caller(0)
	OnFrame(func(*FrameEvent) { frames++ })
	Every(10*time.Millisecond, func() { ticks++ })
	Command("go", func(Player, string) { commands++ })
	t.Cleanup(func() {
		registered.frames = registered.frames[:len(registered.frames)-1]
		sitesOf["OnFrame"] = sitesOf["OnFrame"][:len(sitesOf["OnFrame"])-1]
		delete(registered.commands, "go")
	})
	step := func(seconds float64) {
		if _, err := registered.Command(&CommandEvent{Line: "go"}); err != nil {
			t.Fatal(err)
		}
		if err := registered.Frame(&FrameEvent{TimeSeconds: seconds}); err != nil {
			t.Fatal(err)
		}
	}

	// Without the profiler nothing reads the clock.
	reads := countClock(t)
	for i := 1; i <= 5; i++ {
		step(float64(i))
	}
	if *reads != 0 || frames != 5 || commands != 5 || ticks == 0 {
		t.Fatalf("off: %d clock reads, %d frames, %d commands, %d ticks", *reads, frames, commands, ticks)
	}

	// The first read turns timing on and reports nothing yet.
	if read := profileRead(t, "read"); len(read.Sites) != 0 || read.Frames.Calls != 0 {
		t.Fatalf("first read reported %+v", read)
	}
	for i := 10; i <= 13; i++ {
		step(float64(i))
	}
	if *reads == 0 {
		t.Fatal("profiling read no clock")
	}

	// The next read has each handler's calls at the line that registered it.
	read := profileRead(t, "read")
	if read.Frames.Calls != 4 {
		t.Fatalf("frames = %+v, want 4 calls", read.Frames)
	}
	want := map[string]string{
		"OnFrame":     "profile_test.go:" + strconv.Itoa(line+1),
		"Every":       "profile_test.go:" + strconv.Itoa(line+2),
		"Command /go": "profile_test.go:" + strconv.Itoa(line+3),
	}
	for _, row := range read.Sites {
		if at, ok := want[row.Kind]; ok {
			if !strings.HasSuffix(row.At, at) || row.Calls != 4 {
				t.Errorf("%s at %s with %d calls, want %s with 4", row.Kind, row.At, row.Calls, at)
			}
			delete(want, row.Kind)
		}
	}
	if len(want) != 0 {
		t.Errorf("missing sites %v in %+v", want, read.Sites)
	}

	// Stop turns timing off again.
	profileRead(t, "stop")
	before := *reads
	step(20)
	if *reads != before {
		t.Fatalf("stopped profiler read the clock %d times", *reads-before)
	}
}

func TestProfileListsSlowEvents(t *testing.T) {
	resetClock(t)
	t.Cleanup(func() { profiling = false })

	// The profiler's clock moves only when a handler spends time.
	var fake, spend time.Duration
	base := time.Now()
	held := now
	now = func() time.Time { return base.Add(fake) }
	t.Cleanup(func() { now = held })

	// Register a frame handler and a world handler that spend it.
	_, _, line, _ := runtime.Caller(0)
	OnFrame(func(*FrameEvent) { fake += spend })
	OnWorld(func(string) { fake += spend })
	t.Cleanup(func() {
		registered.frames = registered.frames[:len(registered.frames)-1]
		registered.worlds = registered.worlds[:len(registered.worlds)-1]
		sitesOf["OnFrame"] = sitesOf["OnFrame"][:len(sitesOf["OnFrame"])-1]
		sitesOf["OnWorld"] = sitesOf["OnWorld"][:len(sitesOf["OnWorld"])-1]
	})
	serve := func(method string, event interface{ MarshalVT() ([]byte, error) }) {
		request, err := event.MarshalVT()
		if err != nil {
			t.Fatal(err)
		}
		if reply := serveProfiled(&wasmpb.Call{Method: method, Request: request}); reply.GetError() != "" {
			t.Fatal(reply.GetError())
		}
	}

	// A fast frame, a 6 ms frame, a 5 ms world and twenty more slow frames.
	profileRead(t, "read")
	serve("Frame", &FrameEvent{TimeSeconds: 1})
	spend = 6 * time.Millisecond
	serve("Frame", &FrameEvent{TimeSeconds: 2})
	spend = 5 * time.Millisecond
	serve("World", &WorldEvent{Map: "street_test"})
	spend = 7 * time.Millisecond
	for i := range 20 {
		serve("Frame", &FrameEvent{TimeSeconds: float64(3 + i)})
	}

	// The read keeps 16 events, each with its busiest handler, and counts the
	// rest.
	read := profileRead(t, "read")
	if len(read.Slow) != slowKept || read.SlowMore != 6 {
		t.Fatalf("listed %d slow events and %d more, want %d and 6", len(read.Slow), read.SlowMore, slowKept)
	}
	frame, world := read.Slow[0], read.Slow[1]
	if frame.Event != "Frame" || frame.Ms != 6 || frame.Kind != "OnFrame" || frame.SiteMs != 6 ||
		!strings.HasSuffix(frame.At, "profile_test.go:"+strconv.Itoa(line+1)) {
		t.Errorf("first slow event %+v", frame)
	}
	if world.Event != "World" || world.Ms != 5 || world.Kind != "OnWorld" ||
		!strings.HasSuffix(world.At, "profile_test.go:"+strconv.Itoa(line+2)) {
		t.Errorf("second slow event %+v", world)
	}
	if read.Frames.Over != 21 {
		t.Errorf("%d frames over 4 ms, want 21", read.Frames.Over)
	}

	// The next read starts afresh.
	if again := profileRead(t, "read"); len(again.Slow) != 0 || again.SlowMore != 0 {
		t.Errorf("listed the slow events twice: %+v", again.Slow)
	}
}
