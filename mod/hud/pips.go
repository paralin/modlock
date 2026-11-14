package hud

import (
	"strconv"

	"github.com/paralin/modlock/mod/theme"
	"github.com/paralin/modlock/mod/ui"
)

// Pips shows a count out of a fixed total as filled and empty pips, as the
// stock round counter does.
type Pips struct {
	// Filled is how many pips are lit.
	Filled int
	// Total is how many pips show.
	Total int
	// Tone fills the lit pips.
	Tone theme.Tone
	// Shape is a round point, as the stock round counter, or a narrow tick;
	// Round unless set.
	Shape theme.PipShape
	// Mirror fills from the right, for the far side of a counter.
	Mirror bool
	// Style is drawn over the row's style.
	Style ui.Style
}

// Render returns the row of pips.
func (p Pips) Render() ui.Node {
	// The pips are round unless shaped.
	shape := theme.PipShapes[theme.Round]
	if p.Shape != "" {
		shape = theme.PipShapes[p.Shape]
	}

	// Each pip is lit from the left, or from the right when mirrored.
	pips := make(ui.Nodes, p.Total)
	for i := range pips {
		lit := i < p.Filled
		if p.Mirror {
			lit = i >= p.Total-p.Filled
		}
		fill, ring := theme.EmptyPip, theme.EmptyRing
		if lit {
			fill, ring = string(p.Tone), theme.FilledRing
		}
		pips[i] = ui.Panel{
			Key: "pip-" + strconv.Itoa(i),
			Style: ui.Style{
				Width:        ui.Px(shape.Width),
				Height:       ui.Px(shape.Height),
				BorderRadius: shape.Radius,
				Margin:       ui.Edges{0, 2},
				Background:   fill,
				BorderWidth:  2,
				BorderColor:  ring,
			},
		}
	}
	style := ui.Style{Flow: ui.Right, VerticalAlign: ui.Center}
	return panel(style.Merge(p.Style), pips)
}
