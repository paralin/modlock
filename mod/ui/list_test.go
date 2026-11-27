package ui

import (
	"slices"
	"testing"

	uipb "github.com/paralin/modlock/src/wasm/ui"
)

// TestListSendsOnlyTheRowsThatChanged checks that a list's first draw sends
// every row with its template, and a later draw only the rows added, changed
// and removed, apart from the list's node.
func TestListSendsOnlyTheRowsThatChanged(t *testing.T) {
	list := func(rows ...Row) Node {
		return List{
			Style:     Style{Height: Px(200)},
			RowHeight: 20,
			Row:       Panel{Children: Nodes{Label{Text: "{name}"}, Image{Src: "{icon}"}}},
			Rows:      rows,
		}
	}
	row := func(key, name string) Row {
		return Row{Key: key, Fields: map[string]string{"name": name}}
	}
	v := &view{}

	// The first draw sends the template and every row.
	v.root = list(row("a", "Ann"), row("b", "Bo"))
	first := render(nil, v, v.root, nil)
	change := diff(nil, first)
	node := first.nodes[first.order[len(first.order)-2].Id]
	if node.GetKind() != uipb.Kind_KIND_LIST || len(node.GetList().GetRow()) != 3 || len(node.GetList().GetRows()) != 0 {
		t.Fatalf("list node = %v", node)
	}
	if root := node.GetList().GetRow()[2]; root.GetId() != "" || len(root.GetChildren()) != 2 {
		t.Fatalf("template root = %v", root)
	}
	lists := change.GetLists()
	if len(lists) != 1 || !lists[0].GetReset_() || len(lists[0].GetSet()) != 2 || lists[0].GetNode() != node.GetId() {
		t.Fatalf("first lists = %v", lists)
	}

	// A draw with one row changed, one gone and one new sends those alone.
	v.root = list(row("b", "Bob"), row("c", "Cy"))
	next := render(first, v, v.root, nil)
	change = diff(first, next)
	if len(change.GetSet()) != 0 {
		t.Fatalf("nodes sent again: %v", change.GetSet())
	}
	lists = change.GetLists()
	var keys []string
	for _, each := range lists[0].GetSet() {
		keys = append(keys, each.GetKey())
	}
	if len(lists) != 1 || lists[0].GetReset_() || !slices.Equal(keys, []string{"b", "c"}) || !slices.Equal(lists[0].GetRemoved(), []string{"a"}) {
		t.Fatalf("next lists = %v", lists)
	}

	// The same rows send nothing.
	if change := diff(next, render(next, v, v.root, nil)); change != nil {
		t.Fatalf("unchanged draw sent %v", change)
	}
}
