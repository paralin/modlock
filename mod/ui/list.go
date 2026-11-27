package ui

import (
	"github.com/paralin/modlock/mod"
	uipb "github.com/paralin/modlock/src/wasm/ui"
)

// List scrolls rows far more numerous than an interface's nodes may be, such
// as a leaderboard of every player. The mod holds the rows, and the list's
// node carries only the window of them the player's screen shows, which the
// screen asks for as the player scrolls. So a list of ten thousand rows costs
// a draw and the screen what a list of a hundred does. Each row is drawn from
// the Row template, whose text, images and variables name a row's fields as
// "{field}".
//
// The player focuses a row with the arrow keys or the controller, and chooses
// it with Enter, the controller's confirm or a click, which runs OnChange
// with the row's key. The focus stays on its row as rows come and go. The mod
// keeps the chosen row and draws it back as Selected, which shows while its
// row is in the list.
type List struct {
	// Key keeps the list's node in a list of elements while that list changes.
	Key string
	// Class names the list's classes in the mod's sheets, separated by spaces.
	Class string
	// Style lays out and paints the list; its height is how much it shows.
	Style Style
	// Measure names the list in the placement reports of FollowPlacement.
	Measure string
	// RowHeight is each row's height in pixels, from 1 to 1000.
	RowHeight float32

	// Row is the template each row is drawn from: a panel, label or image
	// holding at most fifteen more, with no buttons or inputs.
	Row Node
	// Rows are the list's rows, from NewRows. Drawing the same Rows again
	// costs the window, not the rows.
	Rows *Rows
	// Selected is the key of the row the mod draws as chosen, or "" for none.
	Selected string
	// FocusStyle is drawn over a row's style while the player focuses it.
	FocusStyle *Style
	// SelectedStyle is drawn over a row's style while it is Selected.
	SelectedStyle *Style
	// OnChange runs with the key of the row the player chose.
	OnChange func(p mod.Player, key string)
}

// listWindow is the rows of a list one player's screen shows: the place of
// the first, how many, and the key of the row with the focus.
type listWindow struct {
	first, count int
	focus        string
}

// defaultWindow is what a list shows before the screen asks for a window.
var defaultWindow = listWindow{count: 50}

// maxWindow is the most rows a window holds.
const maxWindow = 256

// Render returns the list itself.
func (e List) Render() Node {
	return e
}

// key returns the list's key.
func (e List) key() string {
	return e.Key
}

// draw fills the list's node with its template and the window of its rows
// the player's screen shows: the window moved back to end at the last row,
// the list's total, the focused key with its place while the list holds it,
// and the selected key only while the list holds it.
func (e List) draw(l *layout, n *uipb.Node, _ string) {
	// Encode the props every element shares, then the template.
	s := shared{class: e.Class, style: &e.Style, measure: e.Measure}
	s.fill(n)
	n.Kind = uipb.Kind_KIND_LIST
	list := &uipb.List{
		RowHeight:     e.RowHeight,
		Row:           template(e.Row),
		FocusStyle:    wireStyle(e.FocusStyle),
		SelectedStyle: wireStyle(e.SelectedStyle),
	}
	n.List = list
	if e.OnChange != nil {
		l.record(n.Id, handlers{row: e.OnChange})
	}

	// Read the requested window and ordered rows.
	window := l.window(n.Id)
	var order []Row
	var at map[string]int
	if e.Rows != nil {
		order, at = e.Rows.order, e.Rows.at
	}

	// Encode only the visible rows, bounded by maxWindow.
	total := len(order)
	count := min(window.count, maxWindow, total)
	first := min(window.first, total-count)
	list.First, list.Total = uint32(first), uint32(total)
	list.Rows = make([]*uipb.Row, count)
	for i, row := range order[first : first+count] {
		list.Rows[i] = &uipb.Row{Key: row.Key, Fields: row.Fields}
	}

	// Keep focus and selection only where the ordered rows contain them.
	list.Focus = window.focus
	if place, ok := at[window.focus]; ok && window.focus != "" {
		focusAt := uint32(place)
		list.FocusAt = &focusAt
	}
	if _, ok := at[e.Selected]; ok {
		list.Selected = e.Selected
	}
}

// template lays row out as a list's row template: its own nodes, the root
// named "".
func template(row Node) []*uipb.Node {
	return newLayout(nil, nil).template(row)
}

// windowed draws the window of a list a player's screen asks for.
func (l *library) windowed(event *mod.UiWindowEvent) {
	// Keep the window for a list the player's screen holds.
	slot := event.GetPlayer()
	v := l.views[slot]
	if v == nil || !l.retained.hasList(slot, event.GetNode()) {
		return
	}
	if v.windows == nil {
		v.windows = map[string]listWindow{}
	}
	v.windows[event.GetNode()] = listWindow{
		first: int(event.GetFirst()),
		count: min(int(event.GetCount()), maxWindow),
		focus: event.GetFocus(),
	}
	l.draw(v)
}

// forgetWindows drops v's windows of lists s does not hold.
func forgetWindows(v *view, s *screen) {
	for id := range v.windows {
		if s.nodes[id].GetKind() != uipb.Kind_KIND_LIST {
			delete(v.windows, id)
		}
	}
}
