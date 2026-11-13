package hud

import (
	"github.com/paralin/modlock/mod/theme"
	"github.com/paralin/modlock/mod/ui"
)

// TopStrip is the top block of an arena HUD: a heading in spaced capitals, the
// counter pill, and a line under it. Under the kept team bar, the heading and
// pill fit above the stock objective bar and the line goes below that bar.
type TopStrip struct {
	// Heading names the mode or round and the lane, over the counter.
	Heading string
	// Line is a short line under the counter, such as a clock or the news.
	Line string
	// LineTone colors the line; Text unless set.
	LineTone theme.Tone
	// KeepTeamBar seats the strip under the stock team bar, which keeps its
	// clock, heroes and player slots; unset, it takes the bar's place.
	KeepTeamBar bool
	// Children are the counter's content, such as Score and Pips.
	Children ui.Node
}

// Render returns the strip's panels.
func (s TopStrip) Render() ui.Node {
	// The heading sits over the counter pill, and the line reads under both.
	var heading, line ui.Node
	if s.Heading != "" {
		heading = Text{Role: theme.Label, Style: ui.Style{LetterSpacing: 1}, Text: s.Heading}
	}
	if s.Line != "" {
		line = Text{
			Role:  theme.Label,
			Tone:  toneOr(s.LineTone, theme.Text),
			Style: ui.Style{Margin: ui.Edges{4, 0, 0, 0}, FontSize: 18, LetterSpacing: 1},
			Text:  s.Line,
		}
	}
	pill := Pill{Style: ui.Style{Margin: ui.Edges{4, 0, 0, 0}}, Children: s.Children}

	// In the bar's place, one column holds them all; under the kept bar, the
	// line has its own below the objective bar.
	if !s.KeepTeamBar {
		return column(14, ui.Nodes{heading, pill, line})
	}
	var below ui.Node
	if line != nil {
		below = column(theme.ObjectiveBarBottom, line)
	}
	return panel(ui.Style{Width: ui.Fill, Height: ui.Fill}, ui.Nodes{
		column(theme.TeamBarBottom, ui.Nodes{heading, pill}),
		below,
	})
}
