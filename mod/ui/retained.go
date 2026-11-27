package ui

import (
	uipb "github.com/paralin/modlock/src/wasm/ui"
)

// retained keeps only screens whose encoding the host accepted.
// A refused reset discards retention so a later draw starts with a full screen.
type retained struct {
	// screens holds the last accepted screen per slot.
	screens map[int32]*screen
}

// invalidate forgets a slot's screen without changing its requested view.
func (r *retained) invalidate(slot int32) {
	delete(r.screens, slot)
}

// lookup returns a retained node's callbacks, or empty callbacks.
func (r *retained) lookup(slot int32, node string) handlers {
	if s := r.screens[slot]; s != nil {
		return s.handlers[node]
	}
	return handlers{}
}

// hasList reports whether the host accepted a list with this id.
func (r *retained) hasList(slot int32, id string) bool {
	s := r.screens[slot]
	return s != nil && s.nodes[id].GetKind() == uipb.Kind_KIND_LIST
}

// draw sends what changed in the player's interface and notices.
func (r *retained) draw(v *view) bool {
	// Render the player's view and any notices against the last screen sent.
	root := Nodes{v.root, nil}
	if len(v.notices) != 0 {
		root[1] = notices(v.notices)
	}
	slot := v.player.Slot()
	previous := r.screens[slot]
	display := displayRows(v.player, v.display)
	next := render(previous, v, root, display)
	change := diff(previous, next)

	// Send the change; a failed send means the player may have left, so send
	// the screen whole.
	if change != nil && !send(v.player, change) {
		whole := render(nil, v, root, display)
		if !send(v.player, diff(nil, whole)) {
			delete(r.screens, slot)
			return false
		}
		r.screens[slot] = whole
		forgetWindows(v, whole)
		return true
	}

	// Keep the screen the player now holds.
	r.screens[slot] = next
	forgetWindows(v, next)
	return true
}

// screen is what one player's interface holds, as the host last accepted it.
type screen struct {
	// nodes maps each node's id to its encoding, to find what changed.
	nodes map[string]*uipb.Node
	// order lists the nodes in the order laid out, children before parents.
	order []*uipb.Node
	// ids maps each element's place in the tree to its node's id. A place is
	// its parent's place, then "/" for the children, ",index" or ",=key" in a
	// list, and ">" for what a component rendered.
	ids map[string]string
	// handlers maps each button's and input's id to its handlers.
	handlers map[string]handlers
	// next numbers the next new node, so an id is never reused.
	next int
}

// newScreen returns an empty screen that numbers its new nodes from next.
func newScreen(next int) *screen {
	return &screen{
		nodes:    map[string]*uipb.Node{},
		ids:      map[string]string{},
		handlers: map[string]handlers{},
		next:     next,
	}
}

// diff returns the change from previous to next, or nil when nothing changed.
func diff(previous *screen, next *screen) *uipb.Change {
	// Send each node whose encoding changed, and remove each node gone.
	var set []*uipb.Node
	for _, n := range next.order {
		if previous == nil || !previous.nodes[n.Id].EqualVT(n) {
			set = append(set, n)
		}
	}
	var removed []string
	if previous != nil {
		for _, n := range previous.order {
			if _, ok := next.nodes[n.Id]; !ok {
				removed = append(removed, n.Id)
			}
		}
	}
	if previous != nil && len(set) == 0 && len(removed) == 0 {
		return nil
	}
	return &uipb.Change{Reset_: previous == nil, Set: set, Removed: removed}
}
