package mod

import (
	"encoding/json"
	"runtime"
	"strconv"
	"strings"
	"testing"
	"time"
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
