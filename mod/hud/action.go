package hud

import (
	"github.com/paralin/modlock/mod"
	"github.com/paralin/modlock/mod/theme"
	"github.com/paralin/modlock/mod/ui"
)

// Action is a button in the game's menu look: capitals on a fill that lights
// as the pointer enters, with the main menu's hover and press sounds.
type Action struct {
	// Kind is Primary, Secondary or Quiet; Secondary unless set.
	Kind ActionKind
	// Disabled dims the action and takes no presses.
	Disabled bool
	// OnPress runs when the player presses the action.
	OnPress func(p mod.Player)
	// Style is drawn over the action's style.
	Style ui.Style
	// Text is what the action reads.
	Text string
}

// Render returns the action's button.
func (a Action) Render() ui.Node {
	// Each kind has its fill, edge and text, idle and hovered.
	idle, hover, text := ui.Style{
		Background:  theme.Raised,
		BorderWidth: 1,
		BorderColor: theme.Faint,
	}, ui.Style{Background: theme.Lit, BorderColor: string(theme.Text)}, string(theme.Text)
	switch a.Kind {
	case Primary:
		idle, hover, text = ui.Style{Background: string(theme.Text)}, ui.Style{Background: theme.White}, theme.Ink
	case Quiet:
		idle, hover, text = ui.Style{Background: theme.Clear}, ui.Style{Background: theme.Raised}, theme.Quiet
	}

	// A disabled action dims, and neither lights nor ticks under the pointer.
	style := ui.Style{
		Height:       ui.Px(theme.Control),
		Padding:      ui.Edges{0, 14},
		BorderRadius: theme.Radius,
		Transition:   &ui.Transition{Seconds: theme.HoverSeconds},
	}.Merge(idle)
	button := ui.Button{Hover: &hover, HoverSound: theme.HoverSound, Sound: theme.PressSound, Disabled: a.Disabled, OnPress: a.OnPress}
	if a.Disabled {
		style.Opacity = new(float32(0.4))
		button.Hover, button.HoverSound = nil, ""
	}
	button.Style = style.Merge(a.Style)
	button.Children = ui.Label{
		Style: ui.Style{
			Face:            ui.Block,
			FontSize:        16,
			Uppercase:       true,
			LetterSpacing:   1,
			Color:           text,
			HorizontalAlign: ui.Center,
			VerticalAlign:   ui.Center,
			NoWrap:          true,
		},
		Text: a.Text,
	}
	return button
}
