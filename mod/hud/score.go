package hud

import (
	"strconv"

	"github.com/paralin/modlock/mod/theme"
	"github.com/paralin/modlock/mod/ui"
)

// Score is a counter's points, each side in its tone, in the title face. It
// sits in a pill, so its text is plated.
type Score struct {
	// Points are each side's points, the near side first.
	Points [2]int
	// Tones color each side; unset, the near side is the accent and the far
	// side the text.
	Tones [2]theme.Tone
}

// Render returns the row of points.
func (s Score) Render() ui.Node {
	// Each side's points, and the colon between them, leave their place across
	// to the row.
	side := func(points int, tone theme.Tone) ui.Node {
		text := Text{Role: theme.Title, Tone: tone, Plated: true, Style: ui.Style{Margin: ui.Edges{0, 10}}, Text: strconv.Itoa(points)}
		return text.label("")
	}
	colon := Text{Role: theme.Title, Tone: theme.Muted, Plated: true, Text: ":"}
	return panel(ui.Style{Flow: ui.Right, VerticalAlign: ui.Center}, ui.Nodes{
		side(s.Points[0], toneOr(s.Tones[0], theme.Accent)),
		colon.label(""),
		side(s.Points[1], toneOr(s.Tones[1], theme.Text)),
	})
}
