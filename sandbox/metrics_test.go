package sandbox

import (
	"testing"

	"github.com/paralin/modlock/metrics"
	"github.com/paralin/modlock/src/host_app/control"
	"github.com/paralin/modlock/src/wasm"
	"github.com/paralin/modlock/src/wasm/handoff"
		manifestpb "github.com/paralin/modlock/src/wasm/manifest"
	metricspb "github.com/paralin/modlock/src/wasm/metrics"
	"github.com/paralin/modlock/src/wasm/players"
)

// TestMetrics adds up the player's declared metrics and logs a call the
// declaration refuses.
func TestMetrics(t *testing.T) {
	// Declare one labeled count and record what the session logs.
	manifest := &manifestpb.Manifest{Slug: "hud", Metrics: []*metricspb.Metric{
		{Name: "hud.opened", Kind: metricspb.Metric_KIND_COUNT, Labels: []string{"compact"}},
	}}
	var logged []string
	s := &Sandbox{tallies: map[string]*metrics.Tally{}, config: Config{Events: func(event *control.HostEvent) {
		logged = append(logged, event.GetLog().GetText())
	}}}
	add := func(name, label string) {
		request, _ := (&metricspb.AddMetricRequest{Name: name, Label: &label}).MarshalVT()
		s.answer(&Build{Manifest: manifest}, &wasm.Call{Method: "AddMetric", Request: request})
	}

	// Count the declared label and refuse an undeclared label and metric.
	add("hud.opened", "compact")
	add("hud.opened", "compact")
	add("hud.opened", "minimal")
	add("hud.closed", "")
	if got, want := s.tallies["hud"].Text(), "hud.opened[compact] 2"; got != want {
		t.Errorf("totals %q, want %q", got, want)
	}
	if len(logged) != 2 {
		t.Errorf("logged %q, want the two refused calls", logged)
	}
}

// TestHandoff hands the game's and the player's state to the next build, hands
// off none for a bot, and forgets state handed off empty.
func TestHandoff(t *testing.T) {
	s := &Sandbox{config: Config{Events: func(*control.HostEvent) {}}}
	build := &Build{Manifest: &manifestpb.Manifest{Slug: "hud"}}
	call := func(method string, request interface{ MarshalVT() ([]byte, error) }) string {
		data, _ := request.MarshalVT()
		reply := s.answer(build, &wasm.Call{Method: method, Request: data})
		response := &handoff.HandedOffResponse{}
		if err := response.UnmarshalVT(reply.GetResponse()); err != nil {
			t.Fatal(err)
		}
		return response.GetText()
	}
	read := func(slot int32) (string, string) {
		return call("HandedOff", &wasm.Empty{}), call("PlayerHandedOff", &players.PlayerRequest{Player: slot})
	}

	call("Handoff", &handoff.HandoffRequest{Text: "round 2"})
	call("PlayerHandoff", &handoff.HandoffPlayerRequest{Player: 0, Text: "browser open"})
	call("PlayerHandoff", &handoff.HandoffPlayerRequest{Player: 3, Text: "bot"})
	if game, own := read(0); game != "round 2" || own != "browser open" {
		t.Fatalf("read %q and %q, want the handoff state", game, own)
	}
	if _, bot := read(3); bot != "" {
		t.Fatalf("a bot read %q, want none", bot)
	}
	call("PlayerHandoff", &handoff.HandoffPlayerRequest{Player: 0})
	if _, own := read(0); own != "" {
		t.Fatalf("read %q after forgetting, want none", own)
	}
}
