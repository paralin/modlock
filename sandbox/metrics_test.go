package sandbox

import (
	"testing"

	"github.com/paralin/modlock/metrics"
	"github.com/paralin/modlock/proto/modlock/control"
	"github.com/paralin/modlock/proto/modlock/wasm"
)

// TestMetrics adds up the player's declared metrics and logs a call the
// declaration refuses.
func TestMetrics(t *testing.T) {
	// Declare one labeled count and record what the session logs.
	manifest := &wasm.Manifest{Slug: "hud", Metrics: []*wasm.Metric{
		{Name: "hud.opened", Kind: wasm.Metric_KIND_COUNT, Labels: []string{"compact"}},
	}}
	var logged []string
	s := &Sandbox{tallies: map[string]*metrics.Tally{}, config: Config{Events: func(event *control.HostEvent) {
		logged = append(logged, event.GetLog().GetText())
	}}}
	add := func(name, label string) {
		request, _ := (&wasm.AddMetricRequest{Name: name, Label: &label}).MarshalVT()
		s.answer(manifest, &wasm.Call{Method: "AddMetric", Request: request})
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
