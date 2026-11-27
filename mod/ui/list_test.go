package ui

import (
	"fmt"
	"testing"

	"github.com/paralin/modlock/mod"
	uipb "github.com/paralin/modlock/src/wasm/ui"
)

// listNode returns the list node of a screen whose root holds only a list.
func listNode(s *screen) *uipb.Node {
	return s.nodes[s.order[len(s.order)-2].Id]
}

// numbered returns n rows keyed r0 onward, in order.
func numbered(n int) *Rows {
	rows := make([]Row, n)
	for i := range rows {
		rows[i] = Row{Key: fmt.Sprintf("r%05d", i), Fields: map[string]string{"name": fmt.Sprint("Row ", i)}}
	}
	return NewRows(rows...)
}

// TestListSendsTheWindowOfItsRows checks that a list's node carries its
// template and the window of its rows the player's screen asks for, moved
// back to end at the last row, with the focus and selection it holds.
func TestListSendsTheWindowOfItsRows(t *testing.T) {
	v := &view{}
	v.root = List{
		Style:     Style{Height: Px(200)},
		RowHeight: 20,
		Row:       Panel{Children: Nodes{Label{Text: "{name}"}, Image{Src: "{icon}"}}},
		Rows:      numbered(100),
		Selected:  "gone",
	}

	// The first draw sends the template and the default window.
	s := render(nil, v, v.root, nil)
	node := listNode(s)
	list := node.GetList()
	if node.GetKind() != uipb.Kind_KIND_LIST || len(list.GetRow()) != 3 || list.GetRow()[2].GetId() != "" {
		t.Fatalf("list node = %v", node)
	}
	if list.GetFirst() != 0 || list.GetTotal() != 100 || len(list.GetRows()) != 50 || list.GetSelected() != "" {
		t.Fatalf("default window = first %d total %d rows %d selected %q", list.GetFirst(), list.GetTotal(), len(list.GetRows()), list.GetSelected())
	}

	// A window past the end moves back, and the focus it names gets its place.
	v.windows = map[string]listWindow{node.GetId(): {first: 90, count: 20, focus: "r00095"}}
	list = listNode(render(s, v, v.root, nil)).GetList()
	if list.GetFirst() != 80 || len(list.GetRows()) != 20 || list.GetRows()[0].GetKey() != "r00080" || list.GetFocus() != "r00095" || list.GetFocusAt() != 95 {
		t.Fatalf("window = first %d rows %d focus %q at %d", list.GetFirst(), len(list.GetRows()), list.GetFocus(), list.GetFocusAt())
	}

	// A focus the list lacks is echoed with no place.
	v.windows[node.GetId()] = listWindow{count: 10, focus: "gone"}
	if list = listNode(render(s, v, v.root, nil)).GetList(); list.GetFocus() != "gone" || list.FocusAt != nil {
		t.Fatalf("missing focus = %q at %v", list.GetFocus(), list.FocusAt)
	}
}

// TestNewRowsOrdersAndKeepsTheLastOfAKey checks that rows order by rank,
// then sort, then key, and that the last row of a key counts.
func TestNewRowsOrdersAndKeepsTheLastOfAKey(t *testing.T) {
	rows := NewRows(
		Row{Key: "c", Rank: 1},
		Row{Key: "b", Rank: 1, Sort: "a"},
		Row{Key: "a", Rank: 2},
		Row{Key: "d"},
		Row{Key: "c", Rank: -1},
	)
	var keys []string
	for _, row := range rows.order {
		keys = append(keys, row.Key)
	}
	if fmt.Sprint(keys) != "[c d b a]" || rows.at["a"] != 3 {
		t.Fatalf("order = %v, at = %v", keys, rows.at)
	}
}

// TestShowingAListCostsItsWindow checks that drawing a list of 10,000 rows
// allocates what drawing a list of 1,000 does, since the node holds only the
// window.
func TestShowingAListCostsItsWindow(t *testing.T) {
	cost := func(n int) float64 {
		v := &view{}
		rows := numbered(n)
		return testing.AllocsPerRun(20, func() {
			v.root = List{RowHeight: 20, Row: Label{Text: "{name}"}, Rows: rows}
			render(nil, v, v.root, nil)
		})
	}
	small, large := cost(1000), cost(10000)
	if large > small {
		t.Fatalf("allocations: %v rows of 1,000, %v of 10,000", small, large)
	}
}

// TestListDrawsTheWindowTheScreenAsks checks that a window event redraws
// the list with the rows asked for, and that the window drops when the list
// leaves the screen.
func TestListDrawsTheWindowTheScreenAsks(t *testing.T) {
	p := mod.PlayerOf(2)
	v := viewOf(p)
	t.Cleanup(func() { delete(lib.views, 2); delete(lib.screens, 2) })
	v.root = List{RowHeight: 20, Row: Label{Text: "{name}"}, Rows: numbered(1000)}
	s := render(nil, v, v.root, nil)
	lib.screens[2] = s
	id := listNode(s).GetId()

	// A window for a node that is no list is ignored.
	windowed(&mod.UiWindowEvent{Player: 2, Node: "nope", First: 5, Count: 5})
	if len(v.windows) != 0 {
		t.Fatalf("windows = %v", v.windows)
	}

	// The list's window is kept and drawn, whether or not the send lands.
	windowed(&mod.UiWindowEvent{Player: 2, Node: id, First: 500, Count: 300})
	if w := v.windows[id]; w.first != 500 || w.count != maxWindow {
		t.Fatalf("window = %+v", w)
	}
	l := &layout{screen: newScreen(0), windows: v.windows}
	n := &uipb.Node{Id: id}
	v.root.(List).draw(l, n, "")
	if n.GetList().GetFirst() != 500 || len(n.GetList().GetRows()) != maxWindow {
		t.Fatalf("drawn window = first %d rows %d", n.GetList().GetFirst(), len(n.GetList().GetRows()))
	}

	// A screen without the list drops its window.
	v.root = Text("empty")
	forgetWindows(v, render(s, v, v.root, nil))
	if len(v.windows) != 0 {
		t.Fatalf("windows after the list left = %v", v.windows)
	}
}

// TestListHearsTheChosenRow checks that a list encodes its selection and row
// styles, and hands the chosen row's key to OnChange.
func TestListHearsTheChosenRow(t *testing.T) {
	var chosen string
	v := &view{}
	v.root = List{
		RowHeight:     20,
		Row:           Label{Text: "{name}"},
		Rows:          NewRows(Row{Key: "a"}),
		Selected:      "a",
		SelectedStyle: &Style{Bold: true},
		OnChange:      func(_ mod.Player, key string) { chosen = key },
	}
	s := render(nil, v, v.root, nil)
	node := listNode(s)
	if list := node.GetList(); list.GetSelected() != "a" || !list.GetSelectedStyle().GetBold() || list.GetFocusStyle() != nil {
		t.Fatalf("list = %v", list)
	}
	s.handlers[node.GetId()].row(mod.PlayerOf(0), "a")
	if chosen != "a" {
		t.Fatalf("chosen = %q", chosen)
	}
}
