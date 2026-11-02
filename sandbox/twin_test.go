package sandbox

import (
	"bytes"
	"context"
	"fmt"
	"io"
	"math/rand/v2"
	"os"
	"path/filepath"
	"strconv"
	"testing"
	"time"

	"github.com/paralin/modlock/src/wasm"
	"github.com/paralin/modlock/src/wasm/players"
)

// seeded is a deterministic source of random bytes.
type seeded struct{ r *rand.ChaCha8 }

func (s seeded) Read(p []byte) (int, error) { return s.r.Read(p) }

// twinCall is one host call the reference made and the reply it got.
type twinCall struct {
	call  *wasm.Call
	reply *wasm.Reply
}

// TestTwin runs one mod's QuickJS build and its scriptc build in lockstep:
// each event goes to the reference first, whose host calls the sandbox
// answers; then to the candidate, whose calls get the replies of the equal
// reference calls. Both read one virtual clock that moves only between
// events, and the same random bytes. The test fails when any event's calls
// differ, and reports each such event's reordered, missing and extra calls.
//
// PARITY_QUICKJS names a QuickJS build directory, PARITY_WASM a scriptc build
// directory, PARITY_INTERPRETER the quickjs.wasm interpreter.
func TestTwin(t *testing.T) {
	quickjs, scriptc, interpreter := os.Getenv("PARITY_QUICKJS"), os.Getenv("PARITY_WASM"), os.Getenv("PARITY_INTERPRETER")
	if quickjs == "" || scriptc == "" || interpreter == "" {
		t.Skip("set PARITY_QUICKJS, PARITY_WASM and PARITY_INTERPRETER")
	}
	frames, _ := strconv.Atoi(os.Getenv("PARITY_FRAMES"))
	ctx := context.Background()
	read := func(path string) []byte {
		data, err := os.ReadFile(path)
		if err != nil {
			t.Fatal(err)
		}
		return data
	}
	source := read(filepath.Join(quickjs, "mod.js"))

	// One virtual clock for both, in whole milliseconds: scriptc's Date.now
	// adds nanoseconds in floating point and rounds up in the last fraction of
	// a microsecond before each millisecond, which frame steps of 1/60 s hit
	// every time and a real clock almost never.
	now := time.Date(2026, 11, 1, 0, 0, 0, 0, time.UTC)
	clock := func() time.Time { return now.Truncate(time.Millisecond) }

	// The reference records each event's calls; the candidate's calls for the
	// same event are compared with them once both have run.
	var recorded, made []twinCall
	load := func(module []byte, calls func(*wasm.Call) *wasm.Reply, output *bytes.Buffer, random io.Reader) *Mod {
		mod, err := Load(ctx, module, Options{
			Calls:  calls,
			Output: output,
			Clock:  clock,
			Random: random,
		})
		if err != nil {
			t.Fatal(err)
		}
		return mod
	}
	// Both read random_get from one seeded stream. Math.random agrees only
	// when both runtimes draw it straight from random_get.
	var seed [32]byte
	candidateModule := read(interpreter)
	if data, err := os.ReadFile(filepath.Join(scriptc, "mod.wasm")); err == nil {
		candidateModule = data
	}
	var refOut, candOut bytes.Buffer
	reference := load(read(interpreter), func(call *wasm.Call) *wasm.Reply {
		reply := &wasm.Reply{}
		recorded = append(recorded, twinCall{call, reply})
		return reply
	}, &refOut, seeded{rand.NewChaCha8(seed)})
	defer reference.Close(ctx)
	candidate := load(candidateModule, func(call *wasm.Call) *wasm.Reply {
		// The candidate gets the reply of the first unanswered reference call
		// equal to its own, so a reordered call still sees its answer.
		reply := &wasm.Reply{}
		for _, want := range recorded {
			if sameCall(want.call, call) && !answered(made, want.reply) {
				reply = want.reply
				break
			}
		}
		made = append(made, twinCall{call, reply})
		return reply
	}, &candOut, seeded{rand.NewChaCha8(seed)})
	defer candidate.Close(ctx)

	// deliver gives both the event and reports how the candidate's calls
	// differ from the reference's: in order only, or missing and extra calls.
	events, differing := 0, 0
	var report bytes.Buffer
	deliver := func(method string, request message) {
		events++
		recorded, made = recorded[:0], made[:0]
		refErr := reference.Deliver(ctx, method, request, nil)
		candErr := candidate.Deliver(ctx, method, request, nil)
		missing, extra := difference(recorded, made), difference(made, recorded)
		reordered := len(missing) == 0 && len(extra) == 0 && !sameOrder(recorded, made)
		if len(missing) == 0 && len(extra) == 0 && !reordered && (refErr == nil) == (candErr == nil) {
			return
		}
		differing++
		if differing > 20 {
			return
		}
		fmt.Fprintf(&report, "event %d (%s): %d quickjs calls, %d scriptc calls\n", events, method, len(recorded), len(made))
		if (refErr == nil) != (candErr == nil) {
			fmt.Fprintf(&report, "  errors differ: quickjs %v, scriptc %v\n", refErr, candErr)
		}
		if reordered {
			for i := range recorded {
				if !sameCall(recorded[i].call, made[i].call) {
					fmt.Fprintf(&report, "  reordered from call %d: quickjs %s %s, scriptc %s %s\n", i,
						recorded[i].call.GetMethod(), describe(recorded[i].call), made[i].call.GetMethod(), describe(made[i].call))
					break
				}
			}
		}
		for _, c := range missing[:min(len(missing), 5)] {
			fmt.Fprintf(&report, "  only quickjs: %s %s\n", c.call.GetMethod(), describe(c.call))
		}
		for _, c := range extra[:min(len(extra), 5)] {
			fmt.Fprintf(&report, "  only scriptc: %s %s\n", c.call.GetMethod(), describe(c.call))
		}
	}

	deliver("Start", &wasm.StartEvent{Source: source})
	// The candidate reads no source; Start's request differs by design, so
	// the test above gives both the source and scriptc ignores it.
	deliver("PlayersChanged", &players.PlayersChangedEvent{Players: []*players.Connection{player}})
	deliver("World", &wasm.WorldEvent{Map: "dl_midtown"})
	for tick := uint64(1); tick <= uint64(frames); tick++ {
		now = now.Add(frameInterval)
		deliver("Frame", &wasm.FrameEvent{Tick: tick, TimeSeconds: float64(tick) * frameInterval.Seconds()})
	}
	if differing > 0 {
		t.Fatalf("%d of %d events differ:\n%s\nquickjs output:\n%.4000s\nscriptc output:\n%.4000s",
			differing, events, report.String(), refOut.String(), candOut.String())
	}
	t.Logf("%d events matched", events)
}

// sameCall reports whether two calls have the same method and request bytes.
func sameCall(a, b *wasm.Call) bool {
	return a.GetMethod() == b.GetMethod() && bytes.Equal(a.GetRequest(), b.GetRequest())
}

// answered reports whether reply was already given to one of made.
func answered(made []twinCall, reply *wasm.Reply) bool {
	for _, c := range made {
		if c.reply == reply {
			return true
		}
	}
	return false
}

// sameOrder reports whether two equal-length call lists match pairwise.
func sameOrder(a, b []twinCall) bool {
	for i := range a {
		if !sameCall(a[i].call, b[i].call) {
			return false
		}
	}
	return true
}

// difference returns the calls in a that have no equal call left in b,
// counting duplicates.
func difference(a, b []twinCall) []twinCall {
	used := make([]bool, len(b))
	var out []twinCall
	for _, x := range a {
		found := false
		for j, y := range b {
			if !used[j] && sameCall(x.call, y.call) {
				used[j], found = true, true
				break
			}
		}
		if !found {
			out = append(out, x)
		}
	}
	return out
}

// describe prints a call's request, as text when it is a Log line.
func describe(call *wasm.Call) string {
	if call.GetMethod() == "Log" {
		log := &wasm.LogRequest{}
		if log.UnmarshalVT(call.GetRequest()) == nil {
			return strconv.Quote(log.GetMessage())
		}
	}
	return fmt.Sprintf("%d bytes %x", len(call.GetRequest()), call.GetRequest()[:min(len(call.GetRequest()), 64)])
}
