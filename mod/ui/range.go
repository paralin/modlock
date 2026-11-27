package ui

import (
	"github.com/paralin/modlock/mod"
	uipb "github.com/paralin/modlock/src/wasm/ui"
)

// Range is a track with one to eight named handles the player drags, such as
// a minimum, a favored value and a maximum, each a number from Min to Max, 0
// to 1 unless set, stopping at each Step when Step is set. The handles keep
// their order: a handle stops at its neighbors. While the player holds a
// handle a redraw leaves the handles where the player has them, and letting go
// reports every value. Left and right step the active handle from the
// keyboard or a controller, confirm makes the next handle active, and each
// step reports every value at once, as a release. A disabled range takes no
// changes. Changing the handles' names draws a new range.
type Range struct {
	// Key keeps the range's node in a list while the list changes.
	Key string
	// Class names the range's classes in the mod's sheets, separated by spaces.
	Class string
	// Style lays out and paints the range.
	Style Style
	// Hover is drawn over Style while the pointer is over the range.
	Hover *Style
	// Enter is the style the range starts from when it first shows.
	Enter *Style
	// Exit is the style the range eases to as it leaves.
	Exit *Style
	// Cycle swings the range to another style and back while it shows.
	Cycle *Cycle
	// Tooltip is the text the game's tooltip shows over the range.
	Tooltip string
	// Item names an item by class name whose shop card shows over the range.
	Item string
	// Ability names an ability whose window shows over the range.
	Ability *AbilityTip
	// HoverSound is the game's sound event played as the pointer enters.
	HoverSound string
	// Measure names the range in the placement reports of FollowPlacement.
	Measure string
	// Disabled makes the range take no changes.
	Disabled bool
	// Handles are the handles in track order, their values non-decreasing.
	Handles []RangeHandle
	// Min is the least number; with Max also 0 the range is 0 to 1.
	Min float32
	// Max is the greatest number.
	Max float32
	// Step is the distance between the stops, or 0 for none.
	Step float32
	// OnChange runs with every handle's value, in order, when the player lets
	// go of a handle.
	OnChange func(p mod.Player, values []float32)
	// OnHold runs as the player grabs a handle, lets it go, or loses it to a
	// stock overlay, with the handle's name.
	OnHold func(p mod.Player, handle string, phase mod.ActionPhase)
}

// RangeHandle is one handle of a range: its name, 1 to 64 letters, digits,
// '_', '-' or '.', distinct within the range, and its value.
type RangeHandle struct {
	// Name names the handle in the range's reports.
	Name string
	// Value is where the handle sits on the track.
	Value float32
}

// Render returns the range itself.
func (e Range) Render() Node {
	return e
}

// key returns the range's key.
func (e Range) key() string {
	return e.Key
}

// draw fills the range's node.
func (e Range) draw(l *layout, n *uipb.Node, _ string) {
	// Encode the props every element shares, then the element's own.
	s := shared{e.Class, &e.Style, e.Hover, e.Enter, e.Exit, e.Tooltip, e.Item, e.Ability, e.HoverSound, e.Cycle, e.Measure}
	s.fill(n)
	n.Kind = uipb.Kind_KIND_INPUT
	n.Disabled = e.Disabled
	if !e.Disabled && (e.OnChange != nil || e.OnHold != nil) {
		l.screen.handlers[n.Id] = handlers{hold: e.hold}
	}
	handles := make([]*uipb.RangeHandle, len(e.Handles))
	for i, h := range e.Handles {
		handles[i] = &uipb.RangeHandle{Name: h.Name, Value: h.Value}
	}
	n.Input = &uipb.Node_Range{Range: &uipb.Range{
		Handles: handles,
		Min:     e.Min,
		Max:     e.Max,
		Step:    e.Step,
	}}
}

// hold passes each phase of a hold to OnHold, and a release's values to
// OnChange.
func (e Range) hold(p mod.Player, event *mod.RangeEvent) {
	if e.OnHold != nil {
		e.OnHold(p, event.GetHandle(), event.GetPhase())
	}
	if e.OnChange != nil && event.GetPhase() == mod.ActionPhaseReleased {
		e.OnChange(p, event.GetValues())
	}
}
