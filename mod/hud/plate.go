package hud

import (
	"github.com/paralin/modlock/mod/theme"
	"github.com/paralin/modlock/mod/ui"
)

// Plate is the dark plate under HUD text, readable over a bright sky.
type Plate struct {
	// Flow places the children; Down unless set.
	Flow ui.Flow
	// Style is drawn over the plate's style.
	Style ui.Style
	// Children are what the plate holds.
	Children ui.Node
}

// Render returns the plate's panel.
func (b Plate) Render() ui.Node {
	style := ui.Style{
		Flow:            pickFlow(b.Flow, ui.Down),
		HorizontalAlign: ui.Center,
		Background:      theme.PlateColor,
		BorderRadius:    theme.PlateRadius,
		Padding:         theme.PlatePadding,
	}
	return panel(style.Merge(b.Style), b.Children)
}

// pickFlow returns flow, or fallback when flow is unset.
func pickFlow(flow, fallback ui.Flow) ui.Flow {
	if flow == "" {
		return fallback
	}
	return flow
}
