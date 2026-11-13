package ui

import (
	"github.com/paralin/modlock/mod"
	uipb "github.com/paralin/modlock/src/wasm/ui"
)

// Toggle is a box the player checks, labeled by Text. A disabled toggle takes
// no changes.
type Toggle struct {
	// Key keeps the toggle's node in a list while the list changes.
	Key string
	// Class names the toggle's classes in the mod's sheets, separated by spaces.
	Class string
	// Style lays out and paints the toggle.
	Style Style
	// Hover is drawn over Style while the pointer is over the toggle.
	Hover *Style
	// Enter is the style the toggle starts from when it first shows.
	Enter *Style
	// Exit is the style the toggle eases to as it leaves.
	Exit *Style
	// Cycle swings the toggle to another style and back while it shows.
	Cycle *Cycle
	// Tooltip is the text the game's tooltip shows over the toggle.
	Tooltip string
	// Item names an item by class name whose shop card shows over the toggle.
	Item string
	// Ability names an ability whose window shows over the toggle.
	Ability *AbilityTip
	// HoverSound is the game's sound event played as the pointer enters.
	HoverSound string
	// Disabled makes the toggle take no changes.
	Disabled bool
	// Text labels the toggle.
	Text string
	// On checks the box.
	On bool
	// OnChange runs with whether the player checked the box.
	OnChange func(p mod.Player, on bool)
}

// Render returns the toggle itself.
func (e Toggle) Render() Node {
	return e
}

// key returns the toggle's key.
func (e Toggle) key() string {
	return e.Key
}

// draw fills the toggle's node.
func (e Toggle) draw(l *layout, n *uipb.Node, _ string) {
	// Encode the props every element shares, then the element's own.
	s := shared{e.Class, &e.Style, e.Hover, e.Enter, e.Exit, e.Tooltip, e.Item, e.Ability, e.HoverSound, e.Cycle}
	s.fill(n)
	n.Kind = uipb.Kind_KIND_INPUT
	n.Disabled = e.Disabled
	if e.OnChange != nil && !e.Disabled {
		l.screen.handlers[n.Id] = handlers{on: e.OnChange}
	}
	n.Text = e.Text
	n.Input = &uipb.Node_Toggle{Toggle: &uipb.Toggle{On: e.On}}
}
