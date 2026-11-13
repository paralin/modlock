// Package hud draws a mod's HUD in the stock HUD's look, from the tokens in
// package theme. Text sets VALVEPulp capitals over a soft shadow, plates and
// pills hold text over a bright sky, and pips count as the stock round counter
// does. The roster lists the players down the top right corner. Actions are
// the game's menu buttons, with its hover and press sounds, and a meter fills
// toward its value.
//
//	ui.Show(p, hud.TopStrip{Heading: "Round 2", Children: hud.Score{Points: [2]int{1, 0}}})
//
// Each component is a struct whose Render returns the elements of package ui
// that stand for it.
package hud

import (
	"github.com/paralin/modlock/mod/theme"
	"github.com/paralin/modlock/mod/ui"
)

// arriving sets a component's style to ease in from slightly smaller, from
// enter's fields, and to fade out as it goes, over a quarter second. A
// transition the style already holds stays.
func arriving(style *ui.Style, from ui.Style) (enter, exit *ui.Style) {
	*style = ui.Style{Transition: &ui.Transition{Seconds: 0.25, Easing: ui.EaseOut}}.Merge(*style)
	enter = new(ui.Style{Opacity: new(float32(0))}.Merge(from))
	exit = &ui.Style{Opacity: new(float32(0))}
	return enter, exit
}

// panel is a panel with only a style and children, as the kit's components
// build their parts.
func panel(style ui.Style, children ui.Node) ui.Panel {
	return ui.Panel{Style: style, Children: children}
}

// column centers its children from top down, top pixels from its parent's
// top.
func column(top float32, children ui.Node) ui.Panel {
	return panel(ui.Style{Flow: ui.Down, HorizontalAlign: ui.Center, Margin: ui.Edges{top, 0, 0, 0}}, children)
}

// toneOr returns tone, or fallback when tone is unset.
func toneOr(tone, fallback theme.Tone) theme.Tone {
	if tone == "" {
		return fallback
	}
	return tone
}
