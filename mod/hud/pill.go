package hud

import (
	"github.com/paralin/modlock/mod/theme"
	"github.com/paralin/modlock/mod/ui"
)

// Pill is the plate rounded into a capsule with a faint parchment edge,
// flowing right: the counter under a top strip's heading.
type Pill struct {
	// Flow places the children; Right unless set.
	Flow ui.Flow
	// Style is drawn over the pill's style.
	Style ui.Style
	// Children are what the pill holds.
	Children ui.Node
}

// Render returns the pill's panel.
func (b Pill) Render() ui.Node {
	style := ui.Style{
		Flow:            pickFlow(b.Flow, ui.Right),
		HorizontalAlign: ui.Center,
		Background:      theme.PlateColor,
		BorderRadius:    50,
		BorderWidth:     2,
		BorderColor:     theme.PillEdge,
		Padding:         ui.Edges{3, 6},
	}
	return panel(style.Merge(b.Style), b.Children)
}
