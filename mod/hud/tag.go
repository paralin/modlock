package hud

import (
	"github.com/paralin/modlock/mod/theme"
	"github.com/paralin/modlock/mod/ui"
)

// Tag names a status or a result in small heading capitals, in a 1px border
// of its tone.
type Tag struct {
	// Tone colors the border, and the text or the fill.
	Tone theme.Tone
	// Filled fills the tag, for a result; unset, it is outlined, for a status
	// read in a list.
	Filled bool
	// Style is drawn over the tag's style.
	Style ui.Style
	// Text is what the tag reads.
	Text string
}

// Render returns the tag's label.
func (t Tag) Render() ui.Node {
	color, background := string(t.Tone), ""
	if t.Filled {
		color, background = string(theme.OnAccent), string(t.Tone)
	}
	style := ui.Style{
		Face:          ui.Block,
		FontSize:      12,
		LetterSpacing: 1,
		Uppercase:     true,
		Color:         color,
		Background:    background,
		BorderWidth:   1,
		BorderColor:   string(t.Tone),
		BorderRadius:  theme.Radius,
		Padding:       ui.Edges{4, 8},
		VerticalAlign: ui.Center,
	}
	return ui.Label{Style: style.Merge(t.Style), Text: t.Text}
}
