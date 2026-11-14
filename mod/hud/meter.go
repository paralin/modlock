package hud

import (
	"github.com/paralin/modlock/mod/theme"
	"github.com/paralin/modlock/mod/ui"
)

// Meter is a bar that fills from the left toward its value, easing to each new
// value: a charge, a capture or a timer.
type Meter struct {
	// Value is how full the meter is, from 0 to 1.
	Value float32
	// Tone fills the bar; Accent unless set.
	Tone theme.Tone
	// Width is the meter's width in pixels; 160 unless set.
	Width float32
	// Height is the meter's height in pixels; 8 unless set.
	Height float32
	// Style is drawn over the meter's style.
	Style ui.Style
}

// Render returns the meter's panels.
func (m Meter) Render() ui.Node {
	// The meter is 160 by 8 unless sized, and its value stays from 0 to 1.
	width, height := m.Width, m.Height
	if width == 0 {
		width = 160
	}
	if height == 0 {
		height = 8
	}
	value := min(1, max(0, m.Value))

	// The fill scales rather than resizes, so it eases without layout work.
	fill := panel(ui.Style{
		Width:      ui.Percent(100),
		Height:     ui.Percent(100),
		Background: string(toneOr(m.Tone, theme.Accent)),
		Scale:      []float32{value, 1},
		Origin:     []float32{0, 50},
		Transition: &ui.Transition{Seconds: theme.MeterSeconds},
	}, nil)
	style := ui.Style{
		Width:        ui.Px(width),
		Height:       ui.Px(height),
		Background:   theme.Raised,
		BorderRadius: height / 2,
		Overflow:     ui.Clip,
	}
	return panel(style.Merge(m.Style), fill)
}
