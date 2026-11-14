package ui

import (
	"maps"

	uipb "github.com/paralin/modlock/src/wasm/ui"
)

// Label shows text: Text, or the game's text for Token in the player's
// language, with each {s:name} place in it filled from Variables.
type Label struct {
	// Key keeps the label's node in a list while the list changes.
	Key string
	// Class names the label's classes in the mod's sheets, separated by spaces.
	Class string
	// Style lays out and paints the label.
	Style Style
	// Hover is drawn over Style while the pointer is over the label.
	Hover *Style
	// Enter is the style the label starts from when it first shows.
	Enter *Style
	// Exit is the style the label eases to as it leaves.
	Exit *Style
	// Cycle swings the label to another style and back while it shows.
	Cycle *Cycle
	// Tooltip is the text the game's tooltip shows over the label.
	Tooltip string
	// Item names an item by class name whose shop card shows over the label.
	Item string
	// Ability names an ability whose window shows over the label.
	Ability *AbilityTip
	// HoverSound is the game's sound event played as the pointer enters.
	HoverSound string
	// Text is the label's text.
	Text string
	// Token names the game's localized text to show in place of Text.
	Token string
	// Variables fill each {s:name} place in Token's text.
	Variables map[string]string
}

// Render returns the label itself.
func (e Label) Render() Node {
	return e
}

// key returns the label's key.
func (e Label) key() string {
	return e.Key
}

// draw fills the label's node.
func (e Label) draw(_ *layout, n *uipb.Node, _ string) {
	// Encode the props every element shares, then the element's own.
	s := shared{e.Class, &e.Style, e.Hover, e.Enter, e.Exit, e.Tooltip, e.Item, e.Ability, e.HoverSound, e.Cycle}
	s.fill(n)
	n.Kind = uipb.Kind_KIND_LABEL
	n.Text = e.Text
	n.Token = e.Token
	n.Variables = maps.Clone(e.Variables)
}
