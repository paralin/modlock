// Package theme holds the HUD kit's tokens, each named once: the colors, type
// scale and sizes the kit's components draw with, and that a mod's own panels
// can share. They follow the stock HUD's look: VALVEPulp capitals and Retail
// Demo text in off-white with a dark shadow, team colors and translucent
// black plates.
package theme

import "github.com/paralin/modlock/mod/ui"

// Tone is a color a surface may use for meaning. Text, team and danger colors
// are the stock HUD's offWhite, team1Color, team2Color and colorEnemy.
type Tone string

// The tones.
const (
	// Text is the off-white of the HUD's text.
	Text Tone = "#ffefd7"
	// Muted is text that reads under the rest.
	Muted Tone = "#bfb3a1"
	// Accent marks the player's own choices.
	Accent Tone = "#fd736d"
	// OnAccent is text on an accent fill.
	OnAccent Tone = "#070709"
	// Amber is the Hidden King's team color.
	Amber Tone = "#d4860b"
	// Sapphire is the Archmother's team color.
	Sapphire Tone = "#4d75c3"
	// Gas is the gas of a closing arena.
	Gas Tone = "#3ee08f"
	// Success marks done, under par, ready and live: a step darker than gas,
	// so status text and tags sit calmer on the plates.
	Success Tone = "#36c47f"
	// Danger marks an enemy or a loss.
	Danger Tone = "#ff410d"
	// Gold marks a prize.
	Gold Tone = "#ebc934"
)

// TeamTones colors each game team, by its team number: 2 is the Hidden King's
// amber, 3 the Archmother's sapphire.
var TeamTones = map[int32]Tone{2: Amber, 3: Sapphire}

// The shades are the neutrals a full screen draws with, from the stock main
// menu's base styles, so a full screen reads as part of the game's own menus.
// Fills are opaque offWhite tints of the offBlack backdrop: Panorama blends
// translucent fills far stronger than their alpha.
const (
	// Quiet is offWhite at about half strength: idle navigation and secondary
	// text.
	Quiet = "#ffefd78c"
	// Faint is offWhite at the popup button's 44%: idle pill borders.
	Faint = "#ffefd770"
	// Dim is offWhite at about 40%: counts, placeholders and offline players.
	Dim = "#6b675d"
	// Hairline is offWhite at 13%: hairlines between sections and plate edges.
	Hairline = "#2b2a24"
	// Ink is offBlack: text on light fills, and the backdrop's hue.
	Ink = "#10130d"
	// Backdrop is the menu backdrop: an opaque offBlack that hides the stock
	// menu.
	Backdrop = "#0b0d09"
	// Veil is the backdrop at 95%, under a panel the player reads over the
	// game.
	Veil = "#0b0d09f2"
	// Plate is offWhite at 4%: a plate on the backdrop.
	Plate = "#151611"
	// Raised is offWhite at 8%: a row or stat inside a plate.
	Raised = "#1e1f19"
	// Lit is offWhite at 13%: a hovered or joined row.
	Lit = "#2b2a24"
	// White is the full-strength fill of a lit primary button.
	White = "#ffffff"
	// Clear is no fill or border, where a state turns one off.
	Clear = "#00000000"
)

// The shared sizes, in pixels: panel buttons and fields share one height, row
// buttons a shorter one, and every control and tag one corner radius.
const (
	// Control is the height of panel buttons and fields.
	Control = 32
	// RowControl is the height of a button inside a list row.
	RowControl = 28
	// Row is the height of one list row.
	Row = 32
	// Radius rounds buttons, fields and tags.
	Radius = 4
	// PanelRadius rounds panels.
	PanelRadius = 8
)

// Role names one step of the HUD type scale.
type Role string

// The type roles.
const (
	// Display is the largest heading.
	Display Role = "display"
	// Title is a heading or a score.
	Title Role = "title"
	// Body is running text.
	Body Role = "body"
	// Label is a small heading in capitals.
	Label Role = "label"
	// Caption is small running text.
	Caption Role = "caption"
)

// Types gives every build the same typography.
var Types = map[Role]Type{
	Display: {Face: ui.Block, Size: 46, Uppercase: true},
	Title:   {Face: ui.Block, Size: 26, Uppercase: true},
	Body:    {Face: ui.Body, Size: 17},
	Label:   {Face: ui.Block, Size: 15, Uppercase: true},
	Caption: {Face: ui.Body, Size: 14},
}

// The plate is the translucent plate under HUD text. It stays dark enough for
// muted captions to read over a bright sky.
const (
	// PlateColor is the plate's fill.
	PlateColor = "#000000e0"
	// PlateRadius rounds the plate's corners.
	PlateRadius = 5
)

// PlatePadding is the plate's [vertical, horizontal] padding in pixels.
var PlatePadding = ui.Edges{10, 18}

// PillEdge is a pill's resting edge, a faint parchment line.
const PillEdge = "#ffefd71a"

// PipShape is a round point, as in the stock round counter, or a narrow tick.
type PipShape string

// The pip shapes.
const (
	// Round is the stock round counter's point.
	Round PipShape = "round"
	// Tick is a narrow tick.
	Tick PipShape = "tick"
)

// PipShapes are each pip shape's size and corner radius, in pixels.
var PipShapes = map[PipShape]PipSize{
	Round: {Width: 18, Height: 18, Radius: 9},
	Tick:  {Width: 7, Height: 18, Radius: 2},
}

// The stock round counter's point borders, and an unearned pip's fill: clear,
// so only its faint ring shows.
const (
	// EmptyRing is an unearned pip's border.
	EmptyRing = "#ffefd72e"
	// FilledRing is an earned pip's border.
	FilledRing = "#10130de6"
	// EmptyPip is an unearned pip's fill.
	EmptyPip = "#00000000"
)

// Where the stock HUD's parts end, in pixels on a 1080 high HUD.
const (
	// TeamBarBottom is where the stock team bar ends; a strip that keeps the
	// bar sits below it.
	TeamBarBottom = 84
	// ObjectiveBarBottom is where the stock objective bar under the team bar
	// ends, its urn and Rift progress; a strip's line goes below it so the
	// bar's labels stay clear.
	ObjectiveBarBottom = 220
	// FPSReadoutBottom is how far down the top right corner HUD starts: under
	// the game's FPS readout, whose third line, the red slow server frame
	// warning, ends about 62px down.
	FPSReadoutBottom = 68
)

// The game's interface sound events for the kit's controls: the main menu's
// hover tick and press.
const (
	// HoverSound plays as the pointer enters a control.
	HoverSound = "UI.MainMenu.Hover"
	// PressSound plays as the player presses a control.
	PressSound = "UI.MainMenu.Activate"
)

// How long the kit's controls take to answer the pointer and a meter to reach
// its value, in seconds: quick enough to feel immediate.
const (
	// HoverSeconds is how long a control takes to answer the pointer.
	HoverSeconds = 0.15
	// MeterSeconds is how long a meter takes to reach its value.
	MeterSeconds = 0.25
)
