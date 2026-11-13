package ui

import (
	uipb "github.com/paralin/modlock/src/wasm/ui"
)

// Hero shows a picture of a hero, by the game's hero id.
type Hero struct {
	// Key keeps the hero's node in a list while the list changes.
	Key string
	// Class names the hero's classes in the mod's sheets, separated by spaces.
	Class string
	// Style lays out and paints the hero.
	Style Style
	// Hover is drawn over Style while the pointer is over the hero.
	Hover *Style
	// Enter is the style the hero starts from when it first shows.
	Enter *Style
	// Exit is the style the hero eases to as it leaves.
	Exit *Style
	// Cycle swings the hero to another style and back while it shows.
	Cycle *Cycle
	// Tooltip is the text the game's tooltip shows over the hero.
	Tooltip string
	// Item names an item by class name whose shop card shows over the hero.
	Item string
	// Ability names an ability whose window shows over the hero.
	Ability *AbilityTip
	// HoverSound is the game's sound event played as the pointer enters.
	HoverSound string
	// Hero is the hero's id, as Pawn.Hero and mod.Heroes give it.
	Hero uint32
	// Look is which of the game's pictures of the hero to show.
	Look HeroLook
	// Scaling fits the picture into the element's box.
	Scaling Scaling
}

// Render returns the hero itself.
func (e Hero) Render() Node {
	return e
}

// key returns the hero's key.
func (e Hero) key() string {
	return e.Key
}

// draw fills the hero's node.
func (e Hero) draw(_ *layout, n *uipb.Node, _ string) {
	// Encode the props every element shares, then the element's own.
	s := shared{e.Class, &e.Style, e.Hover, e.Enter, e.Exit, e.Tooltip, e.Item, e.Ability, e.HoverSound, e.Cycle}
	s.fill(n)
	n.Kind = uipb.Kind_KIND_HERO_IMAGE
	n.Hero = int32(e.Hero)
	n.HeroLook = heroLook(e.Look)
	n.Scaling = scaling(e.Scaling)
}
