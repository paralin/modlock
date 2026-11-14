package hud

import (
	"github.com/paralin/modlock/mod/theme"
	"github.com/paralin/modlock/mod/ui"
)

// Banner announces a moment: a centered title in the heading face with a
// muted detail under it. It grows in as it appears and fades as it goes.
type Banner struct {
	// Title is the banner's heading.
	Title string
	// Detail is the line under the title, if any.
	Detail string
	// Tone colors the title.
	Tone theme.Tone
	// Style is drawn over the banner's style.
	Style ui.Style
}

// Render returns the banner's panel.
func (b Banner) Render() ui.Node {
	// The title, and the detail when there is one, keep their places.
	var detail ui.Node
	if b.Detail != "" {
		detail = Text{Role: theme.Caption, Tone: theme.Muted, Text: b.Detail}
	}
	style := ui.Style{Flow: ui.Down, HorizontalAlign: ui.Center, Margin: ui.Edges{20, 0, 0, 0}}.Merge(b.Style)
	enter, exit := arriving(&style, ui.Style{Scale: []float32{0.92}})
	return ui.Panel{
		Style:    style,
		Enter:    enter,
		Exit:     exit,
		Children: ui.Nodes{Text{Role: theme.Title, Tone: b.Tone, Text: b.Title}, detail},
	}
}
