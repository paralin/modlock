package ui

import uipb "github.com/paralin/modlock/src/wasm/ui"

// Layout loads a layout from the mod's content addon: Src is its path there,
// such as "shop.xml", and Data is the JSON text its script reads with
// Modlock.data.
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
func (e Layout) draw(_ *layout, n *uipb.Node, _ string) {
	// Encode the props every element shares, then the element's own.
	s := shared{class: e.Class, style: &e.Style}
	s.fill(n)
	n.Kind = uipb.Kind_KIND_LAYOUT
	n.Layout = e.Src
	n.Data = e.Data
}
