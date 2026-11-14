package hud

import (
	"github.com/paralin/modlock/mod/theme"
	"github.com/paralin/modlock/mod/ui"
)

// Text is one label in a type role, over the soft shadow unless plated.
type Text struct {
	// Role is the label's step of the type scale.
	Role theme.Role
	// Tone colors the text; a Label role is muted and the rest are text.
	Tone theme.Tone
	// Plated marks text on a plate or pill, which takes no shadow: on the dark
	// plate a shadow only smears the glyphs.
	Plated bool
	// Style is drawn over the role's style.
	Style ui.Style
	// Text is what the label reads.
	Text string
}

// Render returns the label.
func (t Text) Render() ui.Node {
	return t.label(ui.Center)
}

// label builds the label, placed across by align, or by the renderer when
// align is unset.
func (t Text) label(align ui.Align) ui.Label {
	// The role's type sets the face, size and case; a label reads muted.
	kind := theme.Types[t.Role]
	fallback := theme.Text
	if t.Role == theme.Label {
		fallback = theme.Muted
	}
	style := ui.Style{
		Face:            kind.Face,
		FontSize:        kind.Size,
		Uppercase:       kind.Uppercase,
		Shadow:          !t.Plated,
		Color:           string(toneOr(t.Tone, fallback)),
		HorizontalAlign: align,
		TextAlign:       ui.Center,
	}
	return ui.Label{Style: style.Merge(t.Style), Text: t.Text}
}
