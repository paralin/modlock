package ui

import (
	"maps"
	"slices"
	"strconv"

	uipb "github.com/paralin/modlock/src/wasm/ui"
)

// layout holds one screen while place lays it out.
type layout struct {
	// previous is the screen the player holds, whose ids the layout keeps.
	previous *screen
	// screen is the screen being laid out.
	screen *screen
	// windows holds the player's windows of each list, by node id.
	windows map[string]listWindow
}

// newLayout builds a candidate against the previous accepted screen.
func newLayout(previous *screen, windows map[string]listWindow) *layout {
	next := 0
	if previous != nil {
		next = previous.next
	}
	return &layout{previous: previous, screen: newScreen(next), windows: windows}
}

// record binds a node's callbacks to this candidate screen.
func (l *layout) record(id string, h handlers) {
	l.screen.handlers[id] = h
}

// window returns the requested list window, or the initial window.
func (l *layout) window(id string) listWindow {
	if w, ok := l.windows[id]; ok {
		return w
	}
	return defaultWindow
}

// finish returns the completed screen; the caller takes its lifetime.
func (l *layout) finish() *screen {
	return l.screen
}

// render lays root out as nodes, keeping the ids of elements that stayed in
// place, beside a node for each route.
func render(previous *screen, v *view, root Node, display []*uipb.AbilityRow) *screen {
	// The root is the full-screen panel the elements hang from, and it lists
	// the parts of the game's HUD to hide, whether the cursor is free, the
	// mod's sheets and its last sounds.
	l := newLayout(previous, v.windows)
	l.add(&uipb.Node{
		Id:                "",
		Kind:              uipb.Kind_KIND_PANEL,
		Children:          l.place(root, "/", nil),
		HideHud:           slices.Clone(v.hud),
		FreeCursor:        v.cursor,
		FreeCursorAsks:    v.asks,
		Sheets:            slices.Clone(lib.sheets),
		Cues:              slices.Clone(v.cues),
		Timelines:         slices.Clone(v.timelines),
		AbilityBarPresses: v.bar != nil,
		GameActions:       slices.Sorted(maps.Keys(v.actions)),
		FollowsOverlay:    v.overlay != nil,
		FollowsPlacement:  v.placement != nil,
		Frame:             frame(v.hudFrame),
		AbilityDisplay:    display,
	})

	// A route's id holds a colon, which no element's id does.
	for _, r := range v.routes {
		l.add(&uipb.Node{Id: "route:" + r.name, Kind: uipb.Kind_KIND_ROUTE, Route: r.name, Data: r.data})
	}
	return l.finish()
}

// add records a node.
func (l *layout) add(n *uipb.Node) {
	l.screen.nodes[n.Id] = n
	l.screen.order = append(l.screen.order, n)
}

// id returns the node id for the element at place.
func (l *layout) id(place string) string {
	// Keep the id the place had on the screen the player holds.
	found, ok := "", false
	if l.previous != nil {
		found, ok = l.previous.ids[place]
	}
	if !ok {
		found = strconv.FormatInt(int64(l.screen.next), 36)
		l.screen.next++
	}
	l.screen.ids[place] = found
	return found
}

// place lays out child at place at and appends its nodes' ids to ids.
func (l *layout) place(child Node, at string, ids []string) []string {
	switch c := child.(type) {
	case nil:
		return ids
	case Nodes:
		// A keyed child keeps its place by key, the rest by index.
		for i, item := range c {
			where := at + "," + strconv.Itoa(i)
			if k, ok := item.(keyed); ok && k.key() != "" {
				where = at + ",=" + k.key()
			}
			ids = l.place(item, where, ids)
		}
		return ids
	case Text:
		n := &uipb.Node{Id: l.id(at), Kind: uipb.Kind_KIND_LABEL, Text: string(c)}
		l.add(n)
		return append(ids, n.Id)
	case element:
		n := &uipb.Node{Id: l.id(at)}
		c.draw(l, n, at)
		l.add(n)
		return append(ids, n.Id)
	}

	// A component's nodes hang under its place.
	return l.place(child.Render(), at+">", ids)
}

// template lays out one row and gives its root the template's empty id.
func (l *layout) template(row Node) []*uipb.Node {
	// A row template must have exactly one root.
	ids := l.place(row, "", nil)
	if len(ids) != 1 {
		return nil
	}

	// Preserve child-first order while replacing only the root id.
	for _, n := range l.screen.order {
		if n.Id == ids[0] {
			n.Id = ""
		}
	}
	return l.screen.order
}
