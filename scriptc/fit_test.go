package scriptc

import (
	"bytes"
	"context"
	"os"
	"strings"
	"testing"

	"github.com/paralin/modlock/check"
	"github.com/paralin/modlock/sandbox"
	"github.com/paralin/modlock/src/wasm"
)

// fitted returns testdata/mod.wasm fitted to the boundary. The module is
// scriptc's build of testdata/mod.ts; rebuild it from testdata with
// SCRIPTC_TARGET=wasm32-wasi scriptc build --lib --profile mod.json -o mod.wasm.
func fitted(t *testing.T) []byte {
	// Read the built module and fit it.
	t.Helper()
	module, err := os.ReadFile("testdata/mod.wasm")
	if err != nil {
		t.Fatal(err)
	}
	fit, err := Fit(module)
	if err != nil {
		t.Fatalf("fit the module: %v", err)
	}
	return fit
}

// TestProfile checks that testdata/mod.json, which built testdata/mod.wasm,
// is the profile that builds use.
func TestProfile(t *testing.T) {
	want, err := os.ReadFile("testdata/mod.json")
	if err != nil {
		t.Fatal(err)
	}
	if got := Profile("mod.ts"); !bytes.Equal(got, want) {
		t.Fatalf("Profile(mod.ts) = %s, want testdata/mod.json:\n%s", got, want)
	}
}

// TestFitPassesCheck checks that a fitted module keeps to the host's boundary
// and starts.
func TestFitPassesCheck(t *testing.T) {
	if err := check.Module(context.Background(), fitted(t)); err != nil {
		t.Fatalf("check the fitted module: %v", err)
	}
}

// TestFitEvents delivers Start, which the mod answers after logging through
// host_call, then an event the mod throws on, whose panic must reach the log
// before the mod stops. It runs again with memory that cannot grow, as when a
// mod's heap reaches the host's limit, where the out-of-memory panic must
// still log.
func TestFitEvents(t *testing.T) {
	t.Run("grows", func(t *testing.T) { fitEvents(t, fitted(t), "the test mod answers only Start") })
	t.Run("full", func(t *testing.T) {
		// Find the least memory the mod starts in, so that memory is full
		// when its panic needs a page.
		module := fitted(t)
		for extra := uint32(0); extra < 256; extra++ {
			capped := capMemory(t, module, extra)
			if starts(capped) {
				fitEvents(t, capped, "scriptc: out of memory")
				return
			}
		}
		t.Fatal("the mod does not start in 256 pages more than its initial memory")
	})
}

// starts reports whether module initializes and answers Start twice.
func starts(module []byte) bool {
	ctx := context.Background()
	mod, err := sandbox.Load(ctx, module, sandbox.Options{Calls: func(*wasm.Call) *wasm.Reply {
		return &wasm.Reply{}
	}})
	if err != nil {
		return false
	}
	defer func() { _ = mod.Close(ctx) }()
	for range 2 {
		if mod.Deliver(ctx, "Start", &wasm.StartEvent{}, nil) != nil {
			return false
		}
	}
	return true
}

// capMemory returns module with its memory's maximum set to extra pages more
// than its initial size.
func capMemory(t *testing.T, module []byte, extra uint32) []byte {
	t.Helper()
	r := &reader{data: module, pos: len(header)}
	out := bytes.Clone(header)
	for !r.done() {
		id := r.byte()
		payload := r.bytes(int(r.u32()))
		if id == 5 {
			m := &reader{data: payload}
			count, flags, initial := m.u32(), m.byte(), m.u32()
			if count != 1 || flags&^1 != 0 {
				t.Fatalf("memory section %x, want one 32-bit memory", payload)
			}
			payload = appendU32(appendU32([]byte{1, 1}, initial), initial+extra)
		}
		out = appendSection(out, id, payload)
	}
	if r.err != nil {
		t.Fatal(r.err)
	}
	return out
}

// fitEvents runs TestFitEvents on a fitted module whose panic logs want.
func fitEvents(t *testing.T, module []byte, want string) {
	// Load the fitted module with a host that records each log line.
	ctx := context.Background()
	var logs []string
	mod, err := sandbox.Load(ctx, module, sandbox.Options{Calls: func(call *wasm.Call) *wasm.Reply {
		request := &wasm.LogRequest{}
		if call.GetMethod() == "Log" && request.UnmarshalVT(call.GetRequest()) == nil {
			logs = append(logs, request.GetMessage())
		}
		return &wasm.Reply{}
	}})
	if err != nil {
		t.Fatalf("load the fitted module: %v", err)
	}
	t.Cleanup(func() { _ = mod.Close(ctx) })

	// Start twice: the event wrapper must answer every event, not only the
	// first.
	for range 2 {
		if err := mod.Deliver(ctx, "Start", &wasm.StartEvent{}, nil); err != nil {
			t.Fatalf("deliver Start: %v", err)
		}
	}
	if len(logs) != 2 || logs[0] != "hello from scriptc" {
		t.Fatalf("Start logged %q, want hello from scriptc twice", logs)
	}

	// An event the mod throws on logs the panic and stops the mod.
	logs = nil
	if err := mod.Deliver(ctx, "Command", &wasm.CommandEvent{}, nil); err == nil {
		t.Fatal("Command succeeded, want the mod's panic")
	}
	if len(logs) != 1 || !strings.Contains(logs[0], want) {
		t.Fatalf("the panic logged %q, want %q", logs, want)
	}
	if strings.ContainsAny(logs[0], "\x01\x1f") {
		t.Fatalf("the panic log %q keeps scriptc's separators", logs[0])
	}
	if !mod.Stopped() {
		t.Fatal("the mod still runs after its panic")
	}
}
