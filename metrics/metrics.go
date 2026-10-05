// Package metrics holds the rules for the metrics a mod declares in its
// manifest: which declarations a build may carry, which labels a call may
// name, and how each kind combines the values added to it.
package metrics

import (
	"regexp"
	"slices"
	"strconv"
	"strings"

	"github.com/paralin/modlock/proto/modlock/wasm"
	"github.com/pkg/errors"
)

const (
	// limit bounds how many metrics a mod declares.
	limit = 32
	// labelLimit bounds the labels of one metric.
	labelLimit = 16
)

var (
	// namePattern matches a metric name.
	namePattern = regexp.MustCompile(`^[A-Za-z_][A-Za-z0-9_.]{0,63}$`)
	// labelPattern matches a metric label.
	labelPattern = regexp.MustCompile(`^[A-Za-z0-9_.-]{1,32}$`)
)

// Find returns the metric named name that manifest declares, or nil.
func Find(manifest *wasm.Manifest, name string) *wasm.Metric {
	for _, metric := range manifest.GetMetrics() {
		if metric.GetName() == name {
			return metric
		}
	}
	return nil
}

// Labeled reports whether a call may label metric with label: one of its
// labels, or empty for a metric without labels.
func Labeled(metric *wasm.Metric, label string) bool {
	if len(metric.GetLabels()) == 0 {
		return label == ""
	}
	return slices.Contains(metric.GetLabels(), label)
}

// Validate checks the metrics manifest declares.
func Validate(manifest *wasm.Manifest) error {
	metrics := manifest.GetMetrics()
	if len(metrics) > limit {
		return errors.Errorf("mod.json declares more than %d metrics", limit)
	}
	for i, metric := range metrics {
		if err := validate(metric); err != nil {
			return err
		}
		if slices.ContainsFunc(metrics[:i], func(m *wasm.Metric) bool { return m.GetName() == metric.GetName() }) {
			return errors.Errorf("mod.json declares metric %q twice", metric.GetName())
		}
	}
	return nil
}

// validate checks one declared metric.
func validate(metric *wasm.Metric) error {
	// Check the name the code adds to and how the metric combines values.
	name := metric.GetName()
	if !namePattern.MatchString(name) {
		return errors.Errorf("metric %q name must be up to 64 letters, digits, underscores and dots, starting with a letter or underscore", name)
	}
	switch metric.GetKind() {
	case wasm.Metric_KIND_COUNT, wasm.Metric_KIND_SUM, wasm.Metric_KIND_MAX:
	default:
		return errors.Errorf("metric %q kind must be KIND_COUNT, KIND_SUM or KIND_MAX", name)
	}

	// Check the labels a call may name.
	labels := metric.GetLabels()
	if len(labels) > labelLimit {
		return errors.Errorf("metric %q lists more than %d labels", name, labelLimit)
	}
	for i, label := range labels {
		if !labelPattern.MatchString(label) {
			return errors.Errorf("metric %q label %q must be a short word", name, label)
		}
		if slices.Contains(labels[:i], label) {
			return errors.Errorf("metric %q lists label %q twice", name, label)
		}
	}
	return nil
}

// Tally keeps one player's totals of one mod's metrics.
type Tally struct {
	totals map[key]float64
}

// key names one total.
type key struct {
	name, label string
}

// Add adds value to metric's total under label.
func (t *Tally) Add(metric *wasm.Metric, label string, value float64) {
	// Combine value with the total by the metric's kind.
	if t.totals == nil {
		t.totals = map[key]float64{}
	}
	k := key{metric.GetName(), label}
	total, found := t.totals[k]
	switch metric.GetKind() {
	case wasm.Metric_KIND_COUNT:
		total++
	case wasm.Metric_KIND_SUM:
		total += value
	case wasm.Metric_KIND_MAX:
		if !found || value > total {
			total = value
		}
	}
	t.totals[k] = total
}

// Totals returns the totals in name and label order.
func (t *Tally) Totals() *wasm.MetricTotals {
	// List the totals in a stable order, so one session reads the same way
	// each time.
	keys := make([]key, 0, len(t.totals))
	for k := range t.totals {
		keys = append(keys, k)
	}
	slices.SortFunc(keys, func(a, b key) int {
		return strings.Compare(a.name+"\x00"+a.label, b.name+"\x00"+b.label)
	})
	totals := &wasm.MetricTotals{}
	for _, k := range keys {
		totals.Totals = append(totals.Totals, &wasm.MetricTotal{Name: k.name, Label: k.label, Value: t.totals[k]})
	}
	return totals
}

// Text describes the totals in one line, such as "hud.opened[compact] 3".
func (t *Tally) Text() string {
	var parts []string
	for _, total := range t.Totals().GetTotals() {
		part := total.GetName()
		if total.GetLabel() != "" {
			part += "[" + total.GetLabel() + "]"
		}
		parts = append(parts, part+" "+strconv.FormatFloat(total.GetValue(), 'g', -1, 64))
	}
	return strings.Join(parts, ", ")
}
