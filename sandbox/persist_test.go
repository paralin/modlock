package sandbox

import (
	"slices"
	"strings"
	"testing"

	"github.com/paralin/modlock/persist"
	"github.com/paralin/modlock/src/host_app/control"
	"github.com/paralin/modlock/src/wasm"
	persistpb "github.com/paralin/modlock/src/wasm/persist"
)

// TestPersistService keeps the sandbox player's records in the directory
// the config names, answers started calls from them in the next frame, and
// reads them back in a new sandbox on the same directory, as after a
// restart. A call for a slot with no player is refused.
func TestPersistService(t *testing.T) {
	dir := t.TempDir()
	manifest := &wasm.Manifest{
		Slug:     "prophunt",
		Services: []*wasm.Service{{Name: persist.Service}},
		Persist: []*persistpb.RecordType{{
			Name:    "stats",
			Writers: persistpb.Writers_WRITERS_SERVERS,
			Fields:  map[string]*persistpb.Field{"roundsWon": {Kind: persistpb.Field_KIND_COUNT}},
		}},
	}
	start := func() (*Sandbox, *Build, []string) {
		var logged []string
		s := &Sandbox{config: Config{
			Persist: func(path, slug string) string { return dir },
			Events:  func(event *control.HostEvent) { logged = append(logged, event.GetLog().GetText()) },
		}}
		build := &Build{Manifest: manifest}
		if err := s.provide("build", build); err != nil {
			t.Fatal(err)
		}
		return s, build, logged
	}
	call := func(s *Sandbox, build *Build, method string, request interface{ MarshalVT() ([]byte, error) }) *wasm.ServiceRepliedEvent {
		payload, _ := request.MarshalVT()
		started, _ := (&wasm.StartServiceCallRequest{Service: persist.Service, Method: method, Payload: payload}).MarshalVT()
		calls := &serviceCalls{}
		if reply := s.answer(build, calls, &wasm.Call{Method: "StartServiceCall", Request: started}); reply.GetError() != "" {
			t.Fatalf("start %s: %s", method, reply.GetError())
		}
		if len(calls.ended) != 1 {
			t.Fatalf("%s ended %d times", method, len(calls.ended))
		}
		return calls.ended[0]
	}

	// The first sandbox names where it keeps the records and takes an update.
	s, build, logged := start()
	if want := "service persist: the sandbox, kept in " + dir; !slices.Equal(logged, []string{want}) {
		t.Fatalf("logged %q, want %q", logged, want)
	}
	ended := call(s, build, "Update", &persistpb.UpdateRequest{
		Player: 0, Type: "stats", UpdateKey: "round-1",
		Ops: []*persistpb.Op{{Field: "roundsWon", Op: &persistpb.Op_Add{Add: 1}}},
	})
	if ended.GetError() != "" {
		t.Fatal(ended.GetError())
	}

	// A new sandbox on the directory reads the record back.
	s, build, _ = start()
	ended = call(s, build, "Get", &persistpb.GetRequest{Player: 0, Type: "stats"})
	got := &persistpb.GetReply{}
	if err := got.UnmarshalVT(ended.GetPayload()); err != nil || got.GetRecord().GetFields()["roundsWon"].GetNumber() != 1 {
		t.Fatalf("after restart: %v %v %s", got, err, ended.GetError())
	}

	// Slot 3 holds nobody in the sandbox.
	ended = call(s, build, "Get", &persistpb.GetRequest{Player: 3, Type: "stats"})
	if ended.GetFailure() != wasm.ServiceFailure_SERVICE_FAILURE_REFUSED || !strings.Contains(ended.GetError(), "no player is in slot 3") {
		t.Fatalf("slot 3: %v", ended)
	}
}
