package sandbox

import (
	"testing"

	"github.com/paralin/modlock/src/host_app/control"
	"github.com/paralin/modlock/src/wasm"
	scorespb "github.com/paralin/modlock/src/wasm/scores"
)

// TestCallService refuses a service the sandbox lacks, so a mod takes its
// no-service path instead of decoding an empty reply, and serves scores by
// logging each report its boards accept and refusing the rest.
func TestCallService(t *testing.T) {
	// Declare one partitioned board and record what the session logs.
	var logged []string
	s := &Sandbox{config: Config{Events: func(event *control.HostEvent) {
		logged = append(logged, event.GetLog().GetText())
	}}}
	build := &Build{Manifest: &wasm.Manifest{Slug: "trial", Boards: []*scorespb.Board{{
		Name: "trial.time", Unit: scorespb.Board_UNIT_MILLISECONDS, Order: scorespb.Board_ORDER_ASCENDING,
		Keep: scorespb.Board_KEEP_BEST, Partitions: []*scorespb.Partition{{Name: "course", Values: []string{"canyon"}}},
	}}}}
	serve := func(service, method string, payload interface{ MarshalVT() ([]byte, error) }) *wasm.Reply {
		data, _ := payload.MarshalVT()
		request, _ := (&wasm.ServiceCall{Service: service, Method: method, Payload: data}).MarshalVT()
		return s.answer(build, &wasm.Call{Method: "CallService", Request: request})
	}

	// Refuse a service the sandbox lacks.
	if reply, want := serve("lobby", "setup", &wasm.Empty{}), "this host provides no service lobby"; reply.GetError() != want {
		t.Fatalf("error %q, want %q", reply.GetError(), want)
	}

	// Log a score its board accepts, and refuse an undeclared board and value.
	canyon := map[string]string{"course": "canyon"}
	if reply := serve("scores", "Report", &scorespb.ScoreReport{Board: "trial.time", Player: 0, Value: 41250, Partition: canyon}); reply.GetError() != "" {
		t.Fatalf("report refused: %s", reply.GetError())
	}
	if want := "score trial.time[course=canyon]: player 0 41250"; len(logged) != 1 || logged[0] != want {
		t.Fatalf("logged %q, want %q", logged, want)
	}
	if reply := serve("scores", "Report", &scorespb.ScoreReport{Board: "trial.laps", Partition: canyon}); reply.GetError() == "" {
		t.Fatal("an undeclared board was accepted")
	}
	if reply := serve("scores", "Report", &scorespb.ScoreReport{Board: "trial.time", Partition: map[string]string{"course": "mesa"}}); reply.GetError() == "" {
		t.Fatal("an undeclared course was accepted")
	}

	// Read the board as empty.
	reply := serve("scores", "Read", &scorespb.BoardQuery{Board: "trial.time", Partition: canyon})
	answer := &wasm.ServiceReply{}
	page := &scorespb.BoardPage{}
	if reply.GetError() != "" || answer.UnmarshalVT(reply.GetResponse()) != nil || page.UnmarshalVT(answer.GetPayload()) != nil || len(page.GetRows()) != 0 {
		t.Fatalf("read %v, want an empty page", reply)
	}
}
