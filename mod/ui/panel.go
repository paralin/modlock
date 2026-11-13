package ui

import uipb "github.com/paralin/modlock/src/wasm/ui"

// Panel groups its children.
type Panel struct {
	// Key keeps the panel's node in a list while the list changes.
	Key string
	// Class names the panel's classes in the mod's sheets, separated by spaces.
	Class string
	// Style lays out and paints the panel.
	Style Style
	// Hover is drawn over Style while the pointer is over the panel.
	Hover *Style
	// Enter is the style the panel starts from when it first shows.
	Enter *Style
	// Exit is the style the panel eases to as it leaves.
	Exit *Style
	// Cycle swings the panel to another style and back while it shows.
	Cycle *Cycle
	// Tooltip is the text the game's tooltip shows over the panel.
	Tooltip string
	// Item names an item by class name whose shop card shows over the panel.
	Item string
	// Ability names an ability whose window shows over the panel.
	Ability *AbilityTip
	// HoverSound is the game's sound event played as the pointer enters.
	HoverSound string
	// Children are the nodes the panel holds.
	Children Node
}

// Render returns the panel itself.
func (e Panel) Render() Node {
	return e
}

// key returns the panel's key.
func (e Panel) key() string {
	return e.Key
}

// draw fills the panel's node and lays out its children.
func (e Panel) draw(l *layout, n *uipb.Node, at string) {
	// Encode the props every element shares, then the element's own.
	s := shared{e.Class, &e.Style, e.Hover, e.Enter, e.Exit, e.Tooltip, e.Item, e.Ability, e.HoverSound, e.Cycle}
	s.fill(n)
	n.Kind = uipb.Kind_KIND_PANEL
	n.Children = l.place(e.Children, at+"/", nil)
}
