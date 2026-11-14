package hud

import (
	"strconv"
	"strings"
	"unicode/utf8"

	"github.com/paralin/modlock/mod"
	"github.com/paralin/modlock/mod/theme"
	"github.com/paralin/modlock/mod/ui"
)

// RosterRows is the most players the roster shows, above the minimap.
const RosterRows = 6

// rosterName is the most characters a roster name shows.
const rosterName = 24

// portraitSize is the portrait square's side, its 1px edge included.
const portraitSize = 44

// badgeSize is the side of the avatar badge on a portrait's corner.
const badgeSize = 18

// Roster stacks up to RosterRows players down the top right corner, each one's
// hero portrait at the edge with their name and a line, such as what they are
// doing or their score, to its left. It shows nothing without rows. A mode
// that hides the stock scoreboard's heroes shows who else is here with it.
type Roster struct {
	// Rows are the players, in the order they show.
	Rows []RosterRow
	// Top is the roster's top margin in pixels, under the FPS readout unless
	// set.
	Top *float32
}

// Render returns the roster's panel, or nil without rows.
func (r Roster) Render() ui.Node {
	// The roster shows nothing without rows, and starts under the FPS
	// readout unless placed.
	if len(r.Rows) == 0 {
		return nil
	}
	top := float32(theme.FPSReadoutBottom)
	if r.Top != nil {
		top = *r.Top
	}
	rows := make(ui.Nodes, min(len(r.Rows), RosterRows))
	for i := range rows {
		rows[i] = rosterRow(r.Rows[i], i)
	}
	return panel(ui.Style{
		Width:           ui.FitChildren,
		Height:          ui.FitChildren,
		Flow:            ui.Down,
		HorizontalAlign: ui.Right,
		VerticalAlign:   ui.Top,
		Margin:          ui.Edges{top, 24, 0, 0},
	}, rows)
}

// RosterKey is what the roster shows, to redraw only when it changes.
func RosterKey(rows []RosterRow) string {
	var b strings.Builder
	for i, row := range rows[:min(len(rows), RosterRows)] {
		if i > 0 {
			b.WriteByte('\n')
		}
		if row.Self {
			b.WriteByte('*')
		}
		if row.Selected {
			b.WriteByte('+')
		}
		if row.OnPress != nil {
			b.WriteByte('>')
		}
		b.WriteString(row.Name + "\t" + strconv.FormatUint(uint64(row.Hero), 10) + "\t" + row.Activity + "\t")
		if row.SteamID != 0 {
			b.WriteString(strconv.FormatUint(row.SteamID, 10))
		}
	}
	return b.String()
}

// rosterRow is one player: name and activity, then the portrait, whose edge
// reads in the accent on the viewing player's own row.
func rosterRow(row RosterRow, index int) ui.Node {
	// The name and its line align right, against the portrait.
	right := ui.Style{HorizontalAlign: ui.Right, TextAlign: ui.Right}
	text := panel(ui.Style{Flow: ui.Down, VerticalAlign: ui.Center, Margin: ui.Edges{0, 12, 0, 0}}, ui.Nodes{
		Text{Role: theme.Label, Tone: theme.Text, Style: right, Text: clip(strings.ToUpper(row.Name), rosterName)},
		Text{Role: theme.Caption, Tone: theme.Muted, Style: right.Merge(ui.Style{Margin: ui.Edges{1, 0, 0, 0}}), Text: row.Activity},
	})

	// The portrait sits in a 1px edge on a raised square, empty without a hero,
	// with the player's avatar on its lower left corner.
	var image, badge ui.Node
	if src, ok := portraits[row.Hero]; ok {
		image = ui.Image{Src: src, Style: ui.Style{Width: ui.Percent(100), Height: ui.Percent(100)}}
	}
	if row.SteamID != 0 {
		badge = ui.Avatar{SteamID: row.SteamID, Style: ui.Style{
			Width:           ui.Px(badgeSize),
			Height:          ui.Px(badgeSize),
			HorizontalAlign: ui.Left,
			VerticalAlign:   ui.Bottom,
			BorderRadius:    badgeSize / 2,
			BorderWidth:     1,
			BorderColor:     theme.Dim,
		}}
	}
	edge := theme.Dim
	if row.Self || row.Selected {
		edge = string(theme.Accent)
	}
	portrait := panel(ui.Style{
		Width:         ui.Px(portraitSize),
		Height:        ui.Px(portraitSize),
		Padding:       ui.Edges{1},
		VerticalAlign: ui.Center,
		Background:    edge,
	}, panel(ui.Style{Width: ui.Percent(100), Height: ui.Percent(100), Background: theme.Raised}, ui.Nodes{image, badge}))

	// A player who joins slides in from the edge. A pressable row is a button
	// whose ground lifts under the pointer.
	key := "row-" + strconv.Itoa(index)
	style := ui.Style{Flow: ui.Right, HorizontalAlign: ui.Right, Margin: ui.Edges{0, 0, 8, 0}}
	from := ui.Style{Translate: []float32{16, 0}}
	if row.OnPress == nil {
		enter, exit := arriving(&style, from)
		return ui.Panel{Key: key, Style: style, Enter: enter, Exit: exit, Children: ui.Nodes{text, portrait}}
	}
	style.BorderRadius = theme.Radius
	style.Transition = &ui.Transition{Seconds: theme.HoverSeconds}
	enter, exit := arriving(&style, from)
	return ui.Button{
		Key:        key,
		Style:      style,
		Hover:      &ui.Style{Background: theme.Raised},
		Enter:      enter,
		Exit:       exit,
		HoverSound: theme.HoverSound,
		Sound:      theme.PressSound,
		OnPress:    row.OnPress,
		Children:   ui.Nodes{text, portrait},
	}
}

// portraits maps each hero's id to its small portrait.
var portraits = heroPortraits()

// heroPortraits builds the map of each hero's id to its small portrait.
func heroPortraits() map[uint32]string {
	byID := make(map[uint32]string, len(mod.Heroes))
	for _, hero := range mod.Heroes {
		byID[hero.ID] = hero.Portrait
	}
	return byID
}

// clip shortens text to at most width characters, at a word boundary when
// the cut leaves one, and marks the cut with an ellipsis.
func clip(text string, width int) string {
	if utf8.RuneCountInString(text) <= width {
		return text
	}
	cut := string([]rune(text)[:width-1])
	if space := strings.LastIndexByte(cut, ' '); space > 0 {
		cut = cut[:space]
	}
	return strings.TrimRight(cut, " \t\n") + "…"
}
