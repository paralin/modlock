package ui

import (
	"slices"

	"github.com/paralin/modlock/mod"
	uipb "github.com/paralin/modlock/src/wasm/ui"
)

// Dropdown lets the player pick one of Options; Selected is its index. A
// disabled dropdown takes no changes.
type Dropdown struct {
	// Key keeps the dropdown's node in a list while the list changes.
	Key string
	// Class names the dropdown's classes in the mod's sheets, separated by spaces.
	Class string
	// Style lays out and paints the dropdown.
	Style Style
	// Hover is drawn over Style while the pointer is over the dropdown.
	Hover *Style
	// Enter is the style the dropdown starts from when it first shows.
	Enter *Style
	// Exit is the style the dropdown eases to as it leaves.
	Exit *Style
	// Cycle swings the dropdown to another style and back while it shows.
	Cycle *Cycle
	// Tooltip is the text the game's tooltip shows over the dropdown.
	Tooltip string
	// Item names an item by class name whose shop card shows over the dropdown.
	Item string
	// Ability names an ability whose window shows over the dropdown.
	Ability *AbilityTip
	// HoverSound is the game's sound event played as the pointer enters.
	HoverSound string
	// Disabled makes the dropdown take no changes.
	Disabled bool
	// Options are the choices, in order.
	Options []string
	// Selected is the index of the chosen option.
	Selected int
	// OnChange runs with the index of the option the player picked.
	OnChange func(p mod.Player, index int)
}

// Render returns the dropdown itself.
func (e Dropdown) Render() Node {
	return e
}

// key returns the dropdown's key.
func (e Dropdown) key() string {
	return e.Key
}

// draw fills the dropdown's node.
func (e Dropdown) draw(l *layout, n *uipb.Node, _ string) {
	// Encode the props every element shares, then the element's own.
	s := shared{e.Class, &e.Style, e.Hover, e.Enter, e.Exit, e.Tooltip, e.Item, e.Ability, e.HoverSound, e.Cycle}
	s.fill(n)
	n.Kind = uipb.Kind_KIND_INPUT
	n.Disabled = e.Disabled
	if e.OnChange != nil && !e.Disabled {
		l.screen.handlers[n.Id] = handlers{choice: e.OnChange}
	}
	n.Input = &uipb.Node_Dropdown{Dropdown: &uipb.Dropdown{Options: slices.Clone(e.Options), Selected: uint32(max(0, e.Selected))}}
}
