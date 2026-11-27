package ui

import (
	"maps"
	"slices"

	"github.com/paralin/modlock/mod"
	uipb "github.com/paralin/modlock/src/wasm/ui"
)

// Layout loads a layout from the mod's content addon: Src is its path there,
// such as "shop.xml". The layout keeps the fonts, art and localized strings
// its author drew, and Bindings set parts of its panels by their ids, so a
// layout needs no script. Data is the JSON text a layout's own script reads
// with Modlock.data.
type Layout struct {
	// Key keeps the layout's node in a list while the list changes.
	Key string
	// Class names the layout's classes in the mod's sheets, separated by spaces.
	Class string
	// Style lays out and paints the layout.
	Style Style
	// Src is the layout's path in the mod's content addon.
	Src string
	// Data is the JSON text the layout's script reads, or "" for none.
	Data string
	// Measure names the layout in the placement reports of FollowPlacement.
	Measure string
	// Bindings set parts of the layout's panels, by each panel's id: at most
	// 64. A layout with bindings takes presses only from the bindings with
	// an OnPress.
	Bindings map[string]Binding
}

// Binding sets parts of one panel of a layout and leaves the rest as the
// layout's author drew it. A nil or empty field keeps the author's value, and
// one the mod stops setting returns to it, except Src. A panel the layout
// lacks leaves the binding undrawn and the rest drawn.
type Binding struct {
	// Text replaces a label's words.
	Text *string
	// Token replaces a label's words with the game's text for it in the
	// player's language, each {s:name} place filled from Variables.
	Token     string
	Variables map[string]string
	// Visible shows or hides the panel.
	Visible *bool
	// Class adds classes from the layout's sheets, separated by spaces.
	Class string
	// Style sets inline style; a property it drops returns to the author's.
	Style *Style
	// Src sets an image's picture from the game's content. The game cannot
	// read a picture back, so the last one set stays until the layout loads
	// again.
	Src string
	// OnPress runs when the player presses the panel.
	OnPress func(p mod.Player)
}

// optional returns a pointer to value, or nil when it is empty.
func optional(value string) *string {
	if value == "" {
		return nil
	}
	return &value
}

// Render returns the layout itself.
func (e Layout) Render() Node {
	return e
}

// key returns the layout's key.
func (e Layout) key() string {
	return e.Key
}

// draw fills the layout's node.
func (e Layout) draw(l *layout, n *uipb.Node, _ string) {
	// Encode the props every element shares, then the element's own.
	s := shared{class: e.Class, style: &e.Style, measure: e.Measure}
	s.fill(n)
	n.Kind = uipb.Kind_KIND_LAYOUT
	n.Layout = e.Src
	n.Data = e.Data

	// Encode the bindings in the panels' order, so a redraw sends the same
	// node, and keep each press's handler.
	bound := map[string]func(p mod.Player){}
	for _, panel := range slices.Sorted(maps.Keys(e.Bindings)) {
		b := e.Bindings[panel]
		n.Bindings = append(n.Bindings, &uipb.Binding{
			Panel:     panel,
			Text:      b.Text,
			Token:     optional(b.Token),
			Variables: maps.Clone(b.Variables),
			Visible:   b.Visible,
			Classes:   classes(b.Class),
			Style:     wireStyle(b.Style),
			Image:     optional(b.Src),
			Press:     b.OnPress != nil,
		})
		if b.OnPress != nil {
			bound[panel] = b.OnPress
		}
	}
	if len(bound) != 0 {
		l.screen.handlers[n.Id] = handlers{bound: bound}
	}
}
