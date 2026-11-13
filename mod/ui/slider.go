package ui

import (
	"github.com/paralin/modlock/mod"
	uipb "github.com/paralin/modlock/src/wasm/ui"
)

// Slider is a bar the player drags to a number from Min to Max, 0 to 1 unless
// set, stopping at each Step when Step is set. A disabled slider takes no
// changes.
type Slider struct {
	// Key keeps the slider's node in a list while the list changes.
	Key string
	// Class names the slider's classes in the mod's sheets, separated by spaces.
	Class string
	// Style lays out and paints the slider.
	Style Style
	// Hover is drawn over Style while the pointer is over the slider.
	Hover *Style
	// Enter is the style the slider starts from when it first shows.
	Enter *Style
	// Exit is the style the slider eases to as it leaves.
	Exit *Style
	// Cycle swings the slider to another style and back while it shows.
	Cycle *Cycle
	// Tooltip is the text the game's tooltip shows over the slider.
	Tooltip string
	// Item names an item by class name whose shop card shows over the slider.
	Item string
	// Ability names an ability whose window shows over the slider.
	Ability *AbilityTip
	// HoverSound is the game's sound event played as the pointer enters.
	HoverSound string
	// Disabled makes the slider take no changes.
	Disabled bool
	// Value is the slider's number.
	Value float32
	// Min is the least number; with Max also 0 the range is 0 to 1.
	Min float32
	// Max is the greatest number.
	Max float32
	// Step is the distance between the stops, or 0 for none.
	Step float32
	// OnChange runs with the number the player dragged to.
	OnChange func(p mod.Player, value float32)
	// OnWheel receives one wheel step and its pointer anchor while enabled.
	OnWheel func(p mod.Player, wheel *mod.Wheel)
}

// Render returns the slider itself.
func (e Slider) Render() Node {
	return e
}

// key returns the slider's key.
func (e Slider) key() string {
	return e.Key
}

// draw fills the slider's node.
func (e Slider) draw(l *layout, n *uipb.Node, _ string) {
	// Encode the props every element shares, then the element's own.
	s := shared{e.Class, &e.Style, e.Hover, e.Enter, e.Exit, e.Tooltip, e.Item, e.Ability, e.HoverSound, e.Cycle}
	s.fill(n)
	n.Kind = uipb.Kind_KIND_INPUT
	n.Disabled = e.Disabled
	if !e.Disabled && (e.OnChange != nil || e.OnWheel != nil) {
		l.screen.handlers[n.Id] = handlers{number: e.OnChange, wheel: e.OnWheel}
	}
	n.Input = &uipb.Node_Slider{Slider: &uipb.Slider{
		Value: e.Value,
		Min:   e.Min,
		Max:   e.Max,
		Step:  e.Step,
		Wheel: e.OnWheel != nil && !e.Disabled,
	}}
}
