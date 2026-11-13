// Package ui draws each player's interface from a tree of elements, as struct
// literals:
//
//	ui.Show(p, ui.Panel{
//		Style: ui.Style{Flow: ui.Down},
//		Children: ui.Nodes{
//			ui.Label{Text: "Round 2"},
//			ui.Button{OnPress: ready, Children: ui.Text("Ready")},
//		},
//	})
//
// Each Show lays the tree out as flat nodes whose ids stay with their place in
// the tree, and sends the player only the nodes that changed since the screen
// they hold. Importing the package registers its handlers for presses, input
// changes, the frame clock and the player list with package mod.
package ui

import "github.com/paralin/modlock/mod"

// Node is anything an element may hold: an element, Text, Nodes, a Fragment
// or a component. A component is any other type whose Render returns the
// nodes that stand for it; an element's Render returns the element itself. A
// nil Node holds nothing.
type Node interface {
	// Render returns the nodes that stand for this one.
	Render() Node
}

// Text is a label of plain text.
type Text string

// Render returns the text itself.
func (t Text) Render() Node {
	return t
}

// Nodes is a list of children. A child with a Key keeps its node while the
// list around it changes; the rest keep theirs by index.
type Nodes []Node

// Render returns the list itself.
func (n Nodes) Render() Node {
	return n
}

// keyed is a node that may carry a key in a list.
type keyed interface {
	// key returns the node's key, or "" for none.
	key() string
}

// init registers the library's handlers with package mod. Its frame handler
// runs before a mod's own, since a package's init runs before its importer's.
func init() {
	mod.OnFrame(func(f *mod.FrameEvent) { tick(f.GetTimeSeconds()) })
	mod.OnUiPress(press)
	mod.OnUiInput(change)
	mod.OnPlayers(forget)
}
