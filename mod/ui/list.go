package ui

import (
	"maps"

	uipb "github.com/paralin/modlock/src/wasm/ui"
)

// List scrolls rows far more numerous than an interface's nodes may be, such
// as a leaderboard of every player. The host holds the rows and sends the
// player's screen only the window near what it shows, and the screen builds
// panels only for the rows in view, so a list of ten thousand rows costs the
// screen what a list of a hundred does. Each row is drawn from the Row
// template, whose text, images and variables name a row's fields as
// "{field}". The rows show in order of Rank, then Sort, then Key.
//
// Each draw compares every row with the rows the player holds and sends only
// the rows added, changed and removed, so a draw costs time in proportion to
// the list's rows, and the player's screen only what changed.
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
	// Rows are the list's rows, in any order, each with a distinct Key. The
	// host takes at most 20,000 rows, and 2 MiB of rows across a player's
	// interface.
	Rows []Row
}

// Row is one row of a List.
type Row struct {
	// Key names the row, 1 to 64 bytes, distinct within the list.
	Key string
	// Rank orders the rows, least first.
	Rank float64
	// Sort orders the rows of one rank, as text.
	Sort string
	// Fields are what the template's "{field}" names show, at most sixteen,
	// each named by letters, digits and '_'.
	Fields map[string]string
}

// Render returns the list itself.
func (e List) Render() Node {
	return e
}

// key returns the list's key.
func (e List) key() string {
	return e.Key
}

// draw fills the list's node with its template, and records its rows, which
// diff sends apart from the node.
func (e List) draw(l *layout, n *uipb.Node, _ string) {
	// Encode the props every element shares, then the template.
	s := shared{class: e.Class, style: &e.Style, measure: e.Measure}
	s.fill(n)
	n.Kind = uipb.Kind_KIND_LIST
	n.List = &uipb.List{RowHeight: e.RowHeight, Row: template(e.Row)}

	// Keep the rows by key; of two rows with one key, the last counts.
	rows := &rowSet{keys: make(map[string]*uipb.Row, len(e.Rows))}
	for _, row := range e.Rows {
		if _, ok := rows.keys[row.Key]; !ok {
			rows.order = append(rows.order, row.Key)
		}
		rows.keys[row.Key] = &uipb.Row{Key: row.Key, Rank: row.Rank, Sort: row.Sort, Fields: maps.Clone(row.Fields)}
	}
	l.screen.rows[n.Id] = rows
}

// rowSet is the rows of one list a screen holds.
type rowSet struct {
	// keys holds each row by key.
	keys map[string]*uipb.Row
	// order lists the keys in the order the mod gave the rows.
	order []string
}

// template lays row out as a list's row template: its own nodes, the root
// named "".
func template(row Node) []*uipb.Node {
	t := &layout{screen: newScreen(0)}
	ids := t.place(row, "", nil)
	if len(ids) != 1 {
		return nil
	}
	for _, n := range t.screen.order {
		if n.Id == ids[0] {
			n.Id = ""
		}
	}
	return t.screen.order
}

// listChanges returns the changes that bring each list the player holds on
// previous to its rows on next: every row of a list new to the screen, and
// the rows added, changed and removed of one it holds.
func listChanges(previous, next *screen) []*uipb.ListChange {
	var changes []*uipb.ListChange
	for _, n := range next.order {
		rows, ok := next.rows[n.Id]
		if !ok {
			continue
		}
		held := &rowSet{}
		if previous != nil && previous.rows[n.Id] != nil {
			held = previous.rows[n.Id]
		}
		change := &uipb.ListChange{Node: n.Id, Reset_: held.keys == nil}
		for _, key := range rows.order {
			if row := rows.keys[key]; !held.keys[key].EqualVT(row) {
				change.Set = append(change.Set, row)
			}
		}
		for _, key := range held.order {
			if _, ok := rows.keys[key]; !ok {
				change.Removed = append(change.Removed, key)
			}
		}
		if change.Reset_ || len(change.Set) != 0 || len(change.Removed) != 0 {
			changes = append(changes, change)
		}
	}
	return changes
}
