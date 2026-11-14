package ui

import (
	uipb "github.com/paralin/modlock/src/wasm/ui"
)

// Image shows an image from the game's content, such as
// "file://{images}/hud/icon.png".
type Image struct {
	// Key keeps the image's node in a list while the list changes.
	Key string
	// Class names the image's classes in the mod's sheets, separated by spaces.
	Class string
	// Style lays out and paints the image.
	Style Style
	// Hover is drawn over Style while the pointer is over the image.
	Hover *Style
	// Enter is the style the image starts from when it first shows.
	Enter *Style
	// Exit is the style the image eases to as it leaves.
	Exit *Style
	// Cycle swings the image to another style and back while it shows.
	Cycle *Cycle
	// Tooltip is the text the game's tooltip shows over the image.
	Tooltip string
	// Item names an item by class name whose shop card shows over the image.
	Item string
	// Ability names an ability whose window shows over the image.
	Ability *AbilityTip
	// HoverSound is the game's sound event played as the pointer enters.
	HoverSound string
	// Src is the image's path in the game's content.
	Src string
	// Scaling fits the picture into the image's box.
	Scaling Scaling
}

// Render returns the image itself.
func (e Image) Render() Node {
	return e
}

// key returns the image's key.
func (e Image) key() string {
	return e.Key
}

// draw fills the image's node.
func (e Image) draw(_ *layout, n *uipb.Node, _ string) {
	// Encode the props every element shares, then the element's own.
	s := shared{e.Class, &e.Style, e.Hover, e.Enter, e.Exit, e.Tooltip, e.Item, e.Ability, e.HoverSound, e.Cycle}
	s.fill(n)
	n.Kind = uipb.Kind_KIND_IMAGE
	n.Image = e.Src
	n.Scaling = scaling(e.Scaling)
}
