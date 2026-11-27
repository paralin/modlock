package session

import (
	"cmp"
	"maps"
	"slices"

	"github.com/paralin/modlock/src/wasm/ui"
)

// DefaultWindow is the number of rows a list shows before its renderer asks
// for a window, and MaxWindow the most rows one window holds.
const (
	DefaultWindow = 50
	MaxWindow     = 256
)

// resortAt is the number of rows one change may touch before the list sorts
// all its rows again instead of moving each one.
const resortAt = 16

// rowList holds one list node's rows in order, and the window of them the
// player's renderer asked for.
type rowList struct {
	// order holds the rows by rank, then sort, then key.
	order []*ui.Row
	// keys holds the rows by key.
	keys map[string]*ui.Row
	// first and count are the window: the place of its first row and the
	// number of rows it holds.
	first, count uint32
	// focus is the key of the row the player's renderer last said it focuses,
	// or "" for none.
	focus string
}

// newRowList returns an empty list with the default window.
func newRowList() *rowList {
	return &rowList{keys: make(map[string]*ui.Row), count: DefaultWindow}
}

// compareRows orders rows by rank, then sort, then key.
func compareRows(a, b *ui.Row) int {
	return cmp.Or(
		cmp.Compare(a.GetRank(), b.GetRank()),
		cmp.Compare(a.GetSort(), b.GetSort()),
		cmp.Compare(a.GetKey(), b.GetKey()),
	)
}

// apply applies change to the rows. A change that touches more than resortAt
// rows sorts every row again, in O(n log n); a smaller one moves each row it
// touches, in O(n) for the copy.
func (l *rowList) apply(change *ui.ListChange) {
	// Start over on a reset.
	if change.GetReset_() {
		l.clear()
	}

	// Move each touched row for a small change.
	if len(change.GetSet())+len(change.GetRemoved()) <= resortAt {
		for _, key := range change.GetRemoved() {
			l.remove(key)
		}
		for _, row := range change.GetSet() {
			l.remove(row.GetKey())
			at, _ := slices.BinarySearchFunc(l.order, row, compareRows)
			l.order = slices.Insert(l.order, at, row)
			l.keys[row.GetKey()] = row
		}
		return
	}

	// Sort every row again for a large one.
	for _, key := range change.GetRemoved() {
		delete(l.keys, key)
	}
	for _, row := range change.GetSet() {
		l.keys[row.GetKey()] = row
	}
	l.order = slices.SortedFunc(maps.Values(l.keys), compareRows)
}

// remove drops the row with key, if the list holds it.
func (l *rowList) remove(key string) {
	row := l.keys[key]
	if row == nil {
		return
	}
	if at, found := slices.BinarySearchFunc(l.order, row, compareRows); found {
		l.order = slices.Delete(l.order, at, at+1)
	}
	delete(l.keys, key)
}

// window sets the window to window's run of rows, at most MaxWindow, and
// the focus to the row it names.
func (l *rowList) window(window *ui.ListWindow) {
	l.first, l.count = window.GetFirst(), min(window.GetCount(), MaxWindow)
	l.focus = window.GetFocus()
}

// place returns the place of the row with key, and whether the list holds it.
func (l *rowList) place(key string) (uint32, bool) {
	row := l.keys[key]
	if row == nil {
		return 0, false
	}
	at, found := slices.BinarySearchFunc(l.order, row, compareRows)
	return uint32(at), found
}

// fill returns a copy of the list node with the window's rows, in place of
// any the mod sent. A window past the end moves back to end at the last row.
// The copy shares the rows but not the slice, which later changes shift.
//
// The copy echoes the focus with its row's place, which the renderer reads to
// keep the focus on that row as rows come and go, or, with no place, to move
// it once the row has left. A selected key the list lacks is cleared, so a
// chosen row that left shows nothing chosen.
func (l *rowList) fill(node *ui.Node) *ui.Node {
	// Fill the window's rows.
	total := uint32(len(l.order))
	first := min(l.first, total-min(l.count, total))
	end := min(first+l.count, total)
	filled := node.CloneVT()
	if filled.List == nil {
		filled.List = &ui.List{}
	}
	filled.List.First, filled.List.Total = first, total
	filled.List.Rows = slices.Clone(l.order[first:end])

	// Echo the focus and its place, and drop a selection that left.
	filled.List.Focus = l.focus
	if at, ok := l.place(l.focus); ok {
		filled.List.FocusAt = &at
	}
	if _, ok := l.keys[filled.List.GetSelected()]; !ok {
		filled.List.Selected = ""
	}
	return filled
}

// clear drops every row and keeps the window.
func (l *rowList) clear() {
	l.order, l.keys = nil, make(map[string]*ui.Row)
}

// reset returns a change that brings an empty list to this one's rows.
func (l *rowList) reset(node string) *ui.ListChange {
	return &ui.ListChange{Node: node, Reset_: true, Set: l.order}
}
