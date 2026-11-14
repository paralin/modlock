package ui

import (
	"github.com/paralin/modlock/mod"
	uipb "github.com/paralin/modlock/src/wasm/ui"
)

// Button holds its children and calls OnPress with the player who pressed it.
// Players press buttons while they have a cursor. Sound is the game's sound
// event the press plays, such as "UI.MainMenu.Activate", and a disabled
// button takes no presses.
type Button struct {
	// Key keeps the button's node in a list while the list changes.
	Key string
	// Class names the button's classes in the mod's sheets, separated by spaces.
	Class string
	// Style lays out and paints the button.
	Style Style
	// Hover is drawn over Style while the pointer is over the button.
	Hover *Style
	// Enter is the style the button starts from when it first shows.
	Enter *Style
	// Exit is the style the button eases to as it leaves.
	Exit *Style
	// Cycle swings the button to another style and back while it shows.
	Cycle *Cycle
	// Tooltip is the text the game's tooltip shows over the button.
	Tooltip string
	// Item names an item by class name whose shop card shows over the button.
	Item string
	// Ability names an ability whose window shows over the button.
	Ability *AbilityTip
	// HoverSound is the game's sound event played as the pointer enters.
	HoverSound string
	// Sound is the game's sound event the press plays.
	Sound string
	// Disabled makes the button take no presses.
	Disabled bool
	// OnPress runs with the player who pressed the button.
	OnPress func(p mod.Player)
	// Children are the nodes the button holds.
	Children Node
}

// Render returns the button itself.
func (e Button) Render() Node {
	return e
}

// key returns the button's key.
func (e Button) key() string {
	return e.Key
}

// draw fills the button's node.
func (e Button) draw(l *layout, n *uipb.Node, at string) {
	// Encode the props every element shares, then the element's own.
	s := shared{e.Class, &e.Style, e.Hover, e.Enter, e.Exit, e.Tooltip, e.Item, e.Ability, e.HoverSound, e.Cycle}
	s.fill(n)
	n.Kind = uipb.Kind_KIND_BUTTON
	n.Sound = e.Sound
	n.Disabled = e.Disabled

	// A disabled button's press runs nothing, whoever sends it.
	if e.OnPress != nil && !e.Disabled {
		l.screen.handlers[n.Id] = handlers{press: e.OnPress}
	}
	n.Children = l.place(e.Children, at+"/", nil)
}
