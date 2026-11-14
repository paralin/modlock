package ui

import (
	uipb "github.com/paralin/modlock/src/wasm/ui"
)

// Avatar shows the Steam picture of the player with SteamID.
type Avatar struct {
	// Key keeps the avatar's node in a list while the list changes.
	Key string
	// Class names the avatar's classes in the mod's sheets, separated by spaces.
	Class string
	// Style lays out and paints the avatar.
	Style Style
	// Hover is drawn over Style while the pointer is over the avatar.
	Hover *Style
	// Enter is the style the avatar starts from when it first shows.
	Enter *Style
	// Exit is the style the avatar eases to as it leaves.
	Exit *Style
	// Cycle swings the avatar to another style and back while it shows.
	Cycle *Cycle
	// Tooltip is the text the game's tooltip shows over the avatar.
	Tooltip string
	// Item names an item by class name whose shop card shows over the avatar.
	Item string
	// Ability names an ability whose window shows over the avatar.
	Ability *AbilityTip
	// HoverSound is the game's sound event played as the pointer enters.
	HoverSound string
	// SteamID is whose picture the avatar shows.
	SteamID uint64
}

// Render returns the avatar itself.
func (e Avatar) Render() Node {
	return e
}

// key returns the avatar's key.
func (e Avatar) key() string {
	return e.Key
}

// draw fills the avatar's node.
func (e Avatar) draw(_ *layout, n *uipb.Node, _ string) {
	// Encode the props every element shares, then the element's own.
	s := shared{e.Class, &e.Style, e.Hover, e.Enter, e.Exit, e.Tooltip, e.Item, e.Ability, e.HoverSound, e.Cycle}
	s.fill(n)
	n.Kind = uipb.Kind_KIND_AVATAR
	n.SteamId = e.SteamID
}
