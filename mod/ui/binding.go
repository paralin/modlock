package ui

import "github.com/paralin/modlock/mod"

// Binding sets parts of one panel of a layout and leaves the rest as the
// layout's author drew it. A nil or empty field keeps the author's value, and
// one the mod stops setting returns to it, except Src. A panel the layout
// lacks leaves the binding undrawn and the rest drawn.
type Binding struct {
	// Text replaces a label's words.
	Text *string
	// Token replaces a label's words with the game's text for it in the
	// player's language, each {s:name} place filled from Variables.
	Token string
	// Variables supplies the named values used by the layout.
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
