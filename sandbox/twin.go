package sandbox

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"math/rand/v2"
	"strconv"
	"time"

	"github.com/paralin/modlock/src/wasm"
	"github.com/paralin/modlock/src/wasm/players"
	"github.com/tetratelabs/wazero"
)

// twinEpoch is the time a Twin's clock starts at.
var twinEpoch = time.Date(2026, 1, 1, 0, 0, 0, 0, time.UTC)

// Twin runs two builds of one mod in lockstep, a reference and a candidate,
// and reports the first event on which they differ. A mod's only inputs are
// its events, the replies to its host calls, the clocks and random_get, and
// its only effects are its host calls and its event replies, so two builds
// behave the same exactly when, fed the same inputs, they make the same calls
// and the same replies. The Twin feeds both the same inputs: one clock that
// moves only between events, one stream of random bytes each from one seed,
// and for each of the candidate's calls the reply the reference got to the
// equal call.
//
// Each event goes to the reference first, recording its calls, then to the
// candidate, so a Twin holds one event's calls and takes twice its time.
type Twin struct {
	// reference is the build whose behavior is the oracle.
	reference *Mod
	// candidate is the build checked against it.
	candidate *Mod
	// answer replies to the reference's calls.
	answer func(*wasm.Call) *wasm.Reply
	// now is the clock both builds read.
	now time.Time
	// events counts the events delivered.
	events int
	// recorded holds the reference's calls for the current event.
	recorded []twinCall
	// taken marks each recorded call whose reply the candidate took.
	taken []bool
	// made holds the candidate's calls for the current event.
	made []*wasm.Call
}

// twinCall is one host call the reference made and the reply it got.
type twinCall struct {
	// call is the call.
	call *wasm.Call
	// reply is the answer the reference got.
	reply *wasm.Reply
}

// Divergence is the first event on which a Twin's builds differ, and how.
type Divergence struct {
	// Event counts the events delivered, from 1.
	Event int
	// Method is the event's method.
	Method string
	// Reference is what the reference did where the builds part.
	Reference string
	// Candidate is what the candidate did there.
	Candidate string
}

// Error describes the divergence.
func (d *Divergence) Error() string {
	return fmt.Sprintf("event %d (%s): the reference %s, the candidate %s", d.Event, d.Method, d.Reference, d.Candidate)
}

// NewTwin loads the reference and candidate modules. answer replies to the
// reference's calls, or is nil to answer each empty, as the sandbox does;
// cache keeps compiled modules, or is nil.
func NewTwin(ctx context.Context, reference, candidate []byte, answer func(*wasm.Call) *wasm.Reply, cache wazero.CompilationCache) (*Twin, error) {
	// Hold the clock and the replies both builds read.
	t := &Twin{answer: answer, now: twinEpoch}
	if t.answer == nil {
		t.answer = func(*wasm.Call) *wasm.Reply { return &wasm.Reply{} }
	}
	// scriptc's Date.now adds nanoseconds in floating point and rounds up in
	// the last fraction of a microsecond before a millisecond, which frame
	// steps of 1/60 s hit every time, so the clock reads whole milliseconds.
	clock := func() time.Time { return t.now.Truncate(time.Millisecond) }
	var seed [32]byte

	// Load the reference, which records its calls, then the candidate.
	var err error
	t.reference, err = Load(ctx, reference, Options{Calls: t.record, Cache: cache, Clock: clock, Random: rand.NewChaCha8(seed)})
	if err != nil {
		return nil, err
	}
	t.candidate, err = Load(ctx, candidate, Options{Calls: t.replay, Cache: cache, Clock: clock, Random: rand.NewChaCha8(seed)})
	if err != nil {
		_ = t.reference.Close(ctx)
		return nil, err
	}
	return t, nil
}

// Advance moves both builds' clock forward by d.
func (t *Twin) Advance(d time.Duration) {
	t.now = t.now.Add(d)
}

// Deliver gives the event to both builds and decodes the reference's answer
// into response, which is nil for a method that answers nothing. It returns a
// *Divergence when the builds' calls, answers or failures differ, and
// otherwise the reference's own error.
func (t *Twin) Deliver(ctx context.Context, method string, request message, response message) error {
	// Run the event on the reference, then on the candidate.
	t.events++
	t.recorded, t.taken, t.made = t.recorded[:0], t.taken[:0], t.made[:0]
	var refAnswer, candAnswer raw
	refErr := t.reference.Deliver(ctx, method, request, &refAnswer)
	candErr := t.candidate.Deliver(ctx, method, request, &candAnswer)

	// Compare their calls, then whether each failed, then their answers.
	divergence := func(reference, candidate string) error {
		return &Divergence{Event: t.events, Method: method, Reference: reference, Candidate: candidate}
	}
	if reference, candidate, ok := t.compare(); !ok {
		return divergence(reference, candidate)
	}
	if (refErr == nil) != (candErr == nil) {
		return divergence(outcome(refErr), outcome(candErr))
	}
	if !bytes.Equal(refAnswer, candAnswer) {
		return divergence(fmt.Sprintf("answered %d bytes %x", len(refAnswer), refAnswer), fmt.Sprintf("answered %d bytes %x", len(candAnswer), candAnswer))
	}
	if refErr != nil {
		return refErr
	}
	if response == nil {
		return nil
	}
	return response.UnmarshalVT(refAnswer)
}

// Stopped reports whether a trap or an exhausted budget stopped either build.
func (t *Twin) Stopped() bool {
	return t.reference.Stopped() || t.candidate.Stopped()
}

// Close stops both builds.
func (t *Twin) Close(ctx context.Context) error {
	return errors.Join(t.reference.Close(ctx), t.candidate.Close(ctx))
}

// Play gives the pair what a sandbox session gives a mod: Start with source,
// its players, the world named mapName, then frames frames of the frame clock
// when the mod asks for frames. It stops at the first divergence and returns
// it, ignoring the reference's own errors, as a session logs them and plays
// on.
func (t *Twin) Play(ctx context.Context, source []byte, mapName string, frames int) error {
	// Start the pair and report its player, as Sandbox.start does.
	wants := &wasm.StartResult{}
	if err := t.Deliver(ctx, "Start", &wasm.StartEvent{Source: source}, wants); diverged(err) {
		return err
	}
	if err := t.Deliver(ctx, "PlayersChanged", &players.PlayersChangedEvent{Players: []*players.Connection{player}}, nil); diverged(err) {
		return err
	}
	if wants.GetJoins() {
		if err := t.Deliver(ctx, "PlayerJoined", &players.PlayerJoinedEvent{Connection: player}, nil); diverged(err) {
			return err
		}
	}
	if err := t.Deliver(ctx, "World", &wasm.WorldEvent{Map: mapName}, nil); diverged(err) {
		return err
	}

	// Run the frames while both builds run.
	for tick := uint64(1); wants.GetFrames() && tick <= uint64(frames) && !t.Stopped(); tick++ {
		t.Advance(frameInterval)
		event := &wasm.FrameEvent{Tick: tick, TimeSeconds: float64(tick) * frameInterval.Seconds()}
		if err := t.Deliver(ctx, "Frame", event, nil); diverged(err) {
			return err
		}
	}
	return nil
}

// Events counts the events delivered so far.
func (t *Twin) Events() int {
	return t.events
}

// record answers one of the reference's calls and keeps it.
func (t *Twin) record(call *wasm.Call) *wasm.Reply {
	reply := t.answer(call)
	t.recorded = append(t.recorded, twinCall{call: call, reply: reply})
	t.taken = append(t.taken, false)
	return reply
}

// replay answers one of the candidate's calls with the reply of the first
// equal reference call it has not taken yet, so a call made out of order still
// gets its reply, and keeps it. A call the reference never made answers empty;
// compare reports it once the event ends.
func (t *Twin) replay(call *wasm.Call) *wasm.Reply {
	t.made = append(t.made, call)
	for i, want := range t.recorded {
		if !t.taken[i] && sameCall(want.call, call) {
			t.taken[i] = true
			return want.reply
		}
	}
	return &wasm.Reply{}
}

// compare walks the event's calls in order and describes the first place the
// two lists part, or reports ok when they match. A run of consecutive
// ServerCommand calls compares as a multiset: a mod applies a set of console
// variables by iterating an object, whose key order scriptc does not keep,
// and the game applies them all before its next frame either way.
func (t *Twin) compare() (reference, candidate string, ok bool) {
	i, j := 0, 0
	for i < len(t.recorded) && j < len(t.made) {
		// Compare a run of server commands as a multiset.
		if t.recorded[i].call.GetMethod() == "ServerCommand" && t.made[j].GetMethod() == "ServerCommand" {
			endI, endJ := i, j
			for endI < len(t.recorded) && t.recorded[endI].call.GetMethod() == "ServerCommand" {
				endI++
			}
			for endJ < len(t.made) && t.made[endJ].GetMethod() == "ServerCommand" {
				endJ++
			}
			refRun := make([]*wasm.Call, 0, endI-i)
			for _, c := range t.recorded[i:endI] {
				refRun = append(refRun, c.call)
			}
			if reference, candidate, ok := sameCommands(refRun, t.made[j:endJ]); !ok {
				return reference, candidate, false
			}
			i, j = endI, endJ
			continue
		}

		// Compare one call.
		if !sameCall(t.recorded[i].call, t.made[j]) {
			return "calls " + describe(t.recorded[i].call), "calls " + describe(t.made[j]), false
		}
		i, j = i+1, j+1
	}

	// Report a call only one build made.
	switch {
	case i < len(t.recorded):
		return "calls " + describe(t.recorded[i].call), "calls nothing more", false
	case j < len(t.made):
		return "calls nothing more", "calls " + describe(t.made[j]), false
	}
	return "", "", true
}

// sameCommands compares two runs of ServerCommand calls as multisets and
// describes a command only one run holds.
func sameCommands(reference, candidate []*wasm.Call) (string, string, bool) {
	// Count the reference's commands, then take away the candidate's.
	counts := make(map[string]int, len(reference))
	for _, c := range reference {
		counts[string(c.GetRequest())]++
	}
	for _, c := range candidate {
		key := string(c.GetRequest())
		if counts[key] == 0 {
			return fmt.Sprintf("sends %d server commands", len(reference)), "sends " + describe(c), false
		}
		counts[key]--
	}

	// Report a command the candidate left out.
	for _, c := range reference {
		if counts[string(c.GetRequest())] > 0 {
			return "sends " + describe(c), fmt.Sprintf("sends %d server commands without it", len(candidate)), false
		}
	}
	return "", "", true
}

// sameCall reports whether two calls have the same method and request bytes.
func sameCall(a, b *wasm.Call) bool {
	return a.GetMethod() == b.GetMethod() && bytes.Equal(a.GetRequest(), b.GetRequest())
}

// describe prints a call: as text for a Log line or a server command, and
// otherwise its method and the start of its request.
func describe(call *wasm.Call) string {
	switch call.GetMethod() {
	case "Log":
		line := &wasm.LogRequest{}
		if line.UnmarshalVT(call.GetRequest()) == nil {
			return "Log " + strconv.Quote(line.GetMessage())
		}
	case "ServerCommand":
		command := &wasm.ServerCommandRequest{}
		if command.UnmarshalVT(call.GetRequest()) == nil {
			return "ServerCommand " + strconv.Quote(command.GetCommand())
		}
	}
	request := call.GetRequest()
	return fmt.Sprintf("%s with %d bytes %x", call.GetMethod(), len(request), request[:min(len(request), 32)])
}

// outcome describes how an event ended.
func outcome(err error) string {
	if err == nil {
		return "succeeds"
	}
	return "fails: " + err.Error()
}

// diverged reports whether err is a Twin's divergence.
func diverged(err error) bool {
	var divergence *Divergence
	return errors.As(err, &divergence)
}

// raw is an event answer kept as its bytes.
type raw []byte

// MarshalVT returns the bytes.
func (r raw) MarshalVT() ([]byte, error) {
	return r, nil
}

// UnmarshalVT keeps a copy of data.
func (r *raw) UnmarshalVT(data []byte) error {
	*r = bytes.Clone(data)
	return nil
}
