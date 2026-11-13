package ui

import (
	"github.com/paralin/modlock/mod"
	uipb "github.com/paralin/modlock/src/wasm/ui"
)

// Entry is a box the player types a line into: Value is its text, Placeholder
// the faint text it shows empty, and MaxLength its limit in characters, 100
// unless set, and at most 240 bytes of text. A disabled entry takes no
// changes.
type Entry struct {
	// Key keeps the entry's node in a list while the list changes.
	Key string
	// Class names the entry's classes in the mod's sheets, separated by spaces.
	Class string
	// Style lays out and paints the entry.
	Style Style
	// Hover is drawn over Style while the pointer is over the entry.
	Hover *Style
	// Enter is the style the entry starts from when it first shows.
	Enter *Style
	// Exit is the style the entry eases to as it leaves.
	Exit *Style
	// Cycle swings the entry to another style and back while it shows.
	Cycle *Cycle
	// Tooltip is the text the game's tooltip shows over the entry.
	Tooltip string
	// Item names an item by class name whose shop card shows over the entry.
	Item string
	// Ability names an ability whose window shows over the entry.
	Ability *AbilityTip
	// HoverSound is the game's sound event played as the pointer enters.
	HoverSound string
	// Disabled makes the entry take no changes.
	Disabled bool
	// Value is the entry's text.
	Value string
	// Placeholder is the faint text the entry shows empty.
	Placeholder string
	// MaxLength is the entry's limit in characters.
	MaxLength uint32
	// OnChange runs with the text the player typed.
	OnChange func(p mod.Player, text string)
}

// Render returns the entry itself.
func (e Entry) Render() Node {
	return e
}

// key returns the entry's key.
func (e Entry) key() string {
	return e.Key
}

// draw fills the entry's node.
func (e Entry) draw(l *layout, n *uipb.Node, _ string) {
	// Encode the props every element shares, then the element's own.
	s := shared{e.Class, &e.Style, e.Hover, e.Enter, e.Exit, e.Tooltip, e.Item, e.Ability, e.HoverSound, e.Cycle}
	s.fill(n)
	n.Kind = uipb.Kind_KIND_INPUT
	n.Disabled = e.Disabled
	if e.OnChange != nil && !e.Disabled {
		l.screen.handlers[n.Id] = handlers{text: e.OnChange}
	}
	n.Input = &uipb.Node_TextEntry{TextEntry: &uipb.TextEntry{Text: e.Value, Placeholder: e.Placeholder, MaxLength: e.MaxLength}}
}
