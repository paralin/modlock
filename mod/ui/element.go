package ui

import (
	"strings"

	"github.com/paralin/modlock/mod"
	uipb "github.com/paralin/modlock/src/wasm/ui"
)

// element is a node the renderer draws: it fills its own node.
type element interface {
	Node
	keyed
	// draw fills n, the element's node at place at, and lays out its children.
	draw(l *layout, n *uipb.Node, at string)
}

// shared are the props every drawn element takes, as draw reads them.
type shared struct {
	// class names the element's classes, separated by spaces.
	class string
	// style, hover, enter and exit are the element's styles.
	style, hover, enter, exit *Style
	// tooltip is the text the game's tooltip shows over the element.
	tooltip string
	// item names an item whose shop card shows over the element.
	item string
	// ability names an ability whose window shows over the element.
	ability *AbilityTip
	// hoverSound plays as the pointer enters.
	hoverSound string
	// cycle swings the element to another style and back.
	cycle *Cycle
}

// fill encodes the props every drawn element shares into n.
func (s *shared) fill(n *uipb.Node) {
	// The style and the hover style over it.
	n.Style = wireStyle(s.style)
	n.Hover = wireStyle(s.hover)

	// The game's windows over the element: a tooltip, an item's shop card
	// and an ability's window.
	n.Tooltip = s.tooltip
	if s.item != "" {
		n.Item = mod.ItemID(s.item)
	}
	if s.ability != nil && s.ability.Name != "" {
		n.Ability = mod.ItemID(s.ability.Name)
		n.AbilityPlayer = uint32(s.ability.Player.Slot())
	}

	// The sound, the motion in and out, and the classes.
	n.HoverSound = s.hoverSound
	n.Enter = wireStyle(s.enter)
	n.Exit = wireStyle(s.exit)
	n.Cycle = cycle(s.cycle)
	n.Classes = classes(s.class)
}

// classes splits an element's class into its names, or nil for none.
func classes(names string) []string {
	if strings.TrimSpace(names) == "" {
		return nil
	}
	return strings.Fields(names)
}

// cycle encodes a cycle.
func cycle(value *Cycle) *uipb.Cycle {
	if value == nil {
		return nil
	}
	return &uipb.Cycle{Style: wireStyle(&value.Style), Seconds: value.Seconds, Count: value.Count}
}
