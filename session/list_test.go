package session

import (
	"slices"
	"strconv"
	"testing"

	"github.com/paralin/modlock/src/host_app/control"
	"github.com/paralin/modlock/src/wasm/ui"
)

// listShown returns the event that shows player slot of mod a list node
// "scores" and sets rows keyed from first to first+count, ranked by place.
func listShown(slot int32, mod string, reset bool, first, count int) *control.HostEvent {
	rows := &ui.ListChange{Node: "scores", Reset_: reset}
	for i := first; i < first+count; i++ {
		key := "row" + strconv.Itoa(i)
		rows.Set = append(rows.Set, &ui.Row{Key: key, Rank: float64(i), Fields: map[string]string{"name": key}})
	}
	node := &ui.Node{Id: "scores", Kind: ui.Kind_KIND_LIST, List: &ui.List{RowHeight: 32}}
	change := &ui.Change{Reset_: reset, Set: []*ui.Node{node}, Lists: []*ui.ListChange{rows}}
	return &control.HostEvent{Body: &control.HostEvent_Ui{Ui: &control.UiChanged{Mod: mod, Slot: slot, Change: change}}}
}

// listWindow returns the event that asks for count rows of mod's list
// "scores" from first, for the player in slot.
func listWindow(slot int32, mod string, first, count uint32) *control.HostEvent {
	window := &ui.ListWindow{Node: "scores", First: first, Count: count}
	return &control.HostEvent{Body: &control.HostEvent_UiWindow{UiWindow: &control.UiWindow{Mod: mod, Slot: slot, Window: window}}}
}

// shownList returns the list node "scores" of mod on screen.
func shownList(t *testing.T, screen *ui.Screen) *ui.List {
	t.Helper()
	for _, tree := range screen.GetTrees() {
		for _, node := range tree.GetNodes() {
			if node.GetId() == "scores" {
				return node.GetList()
			}
		}
	}
	t.Fatalf("no list on %v", screen)
	return nil
}

// keys returns the keys of list's rows, in order.
func keys(list *ui.List) []string {
	var keys []string
	for _, row := range list.GetRows() {
		keys = append(keys, row.GetKey())
	}
	return keys
}

// TestScreensWindowAListOfAnySize shows a list of 100 rows and one of 10,000,
// and checks that both screens carry the same window of rows, so what the
// renderer receives and builds does not grow with the list.
func TestScreensWindowAListOfAnySize(t *testing.T) {
	for _, size := range []int{100, 10000} {
		// The list starts with the default window.
		screens := NewScreens(1)
		screens.Handle(joined(0, 1))
		screens.Handle(listShown(0, "arena", true, 0, size))
		screen, _ := screens.Screen()
		list := shownList(t, screen)
		if list.GetTotal() != uint32(size) || len(list.GetRows()) != DefaultWindow || list.GetFirst() != 0 {
			t.Fatalf("%d rows: the first screen held %d rows from %d of %d", size, len(list.GetRows()), list.GetFirst(), list.GetTotal())
		}

		// A window holds the rows it asks for; one past the end ends at the
		// last row, and one too large holds MaxWindow.
		screens.Handle(listWindow(0, "arena", 40, 20))
		screen, _ = screens.Screen()
		if got := keys(shownList(t, screen)); len(got) != 20 || got[0] != "row40" || got[19] != "row59" {
			t.Fatalf("%d rows: the window held %v", size, got)
		}
		screens.Handle(listWindow(0, "arena", uint32(size), 10))
		screen, _ = screens.Screen()
		if list := shownList(t, screen); list.GetFirst() != uint32(size-10) || len(list.GetRows()) != 10 {
			t.Fatalf("%d rows: the window past the end started at %d", size, list.GetFirst())
		}
		screens.Handle(listWindow(0, "arena", 0, 1000))
		screen, _ = screens.Screen()
		if rows := len(shownList(t, screen).GetRows()); rows != min(MaxWindow, size) {
			t.Fatalf("%d rows: a large window held %d rows", size, rows)
		}
	}
}

// TestScreensKeepRowsInOrder moves, removes and adds rows in small and large
// changes, and checks the order by rank, then sort, then key.
func TestScreensKeepRowsInOrder(t *testing.T) {
	screens := NewScreens(1)
	screens.Handle(joined(0, 1))
	screens.Handle(listShown(0, "arena", true, 0, 5))
	held, _ := screens.Screen()

	// A small change moves a row to the top and drops another.
	moved := listShown(0, "arena", false, 0, 0)
	rows := moved.GetUi().GetChange().GetLists()[0]
	rows.Set = []*ui.Row{{Key: "row3", Rank: -1}, {Key: "tie", Rank: 2, Sort: "a"}}
	rows.Removed = []string{"row1"}
	screens.Handle(moved)
	screen, _ := screens.Screen()
	want := []string{"row3", "row0", "row2", "tie", "row4"}
	if got := keys(shownList(t, screen)); !slices.Equal(got, want) {
		t.Fatalf("after a small change the rows were %v, not %v", got, want)
	}

	// The earlier screen is unchanged, since a stream compares against it.
	if got := keys(shownList(t, held)); !slices.Equal(got, []string{"row0", "row1", "row2", "row3", "row4"}) {
		t.Fatalf("a change rewrote an earlier screen's rows: %v", got)
	}

	// A large change sorts every row again with the same order.
	large := listShown(0, "arena", false, 10, 20)
	large.GetUi().GetChange().GetLists()[0].Removed = []string{"row0", "tie"}
	screens.Handle(large)
	screen, _ = screens.Screen()
	got := keys(shownList(t, screen))
	if len(got) != 23 || got[0] != "row3" || got[1] != "row2" || got[2] != "row4" || got[3] != "row10" {
		t.Fatalf("after a large change the rows were %v", got)
	}

	// Rows a mod puts in the node itself never reach the screen.
	carried := listShown(0, "arena", false, 0, 0)
	carried.GetUi().GetChange().GetSet()[0].List.Rows = []*ui.Row{{Key: "forged"}}
	screens.Handle(carried)
	screen, _ = screens.Screen()
	if got := keys(shownList(t, screen)); got[0] == "forged" {
		t.Fatalf("a row the mod set in the node was shown: %v", got)
	}
}

// TestScreensSnapshotCarriesRowsAndWindows brings new Screens to a list's
// rows and window from a snapshot, as a host's daemon starts.
func TestScreensSnapshotCarriesRowsAndWindows(t *testing.T) {
	screens := NewScreens(1)
	screens.Handle(joined(0, 1))
	screens.Handle(listShown(0, "arena", true, 0, 300))
	screens.Handle(listWindow(0, "arena", 120, 30))
	want, _ := screens.Screen()

	copied := NewScreens(1)
	for _, event := range screens.Snapshot() {
		copied.Handle(event)
	}
	got, _ := copied.Screen()
	if !got.EqualVT(want) {
		t.Fatalf("the snapshot's screen differs: %v", keys(shownList(t, got)))
	}

	// A tree's reset drops its rows and keeps its window.
	copied.Handle(listShown(0, "arena", true, 0, 0))
	got, _ = copied.Screen()
	if list := shownList(t, got); list.GetTotal() != 0 || len(list.GetRows()) != 0 {
		t.Fatalf("a reset kept %d rows", list.GetTotal())
	}
	copied.Handle(listShown(0, "arena", false, 0, 300))
	got, _ = copied.Screen()
	if list := shownList(t, got); list.GetFirst() != 120 {
		t.Fatalf("a reset lost the window: %d", list.GetFirst())
	}
}

// TestScreensEchoTheFocusAndDropALostSelection focuses a row, moves rows
// around it and removes it, and checks that the screen echoes the focus with
// the row's place while the list holds it, and a selection only while its row
// stays.
func TestScreensEchoTheFocusAndDropALostSelection(t *testing.T) {
	// Focus row3 and select it.
	screens := NewScreens(1)
	screens.Handle(joined(0, 1))
	shown := listShown(0, "arena", true, 0, 5)
	shown.GetUi().GetChange().GetSet()[0].List.Selected = "row3"
	screens.Handle(shown)
	focused := listWindow(0, "arena", 0, 50)
	focused.GetUiWindow().Window.Focus = "row3"
	screens.Handle(focused)
	screen, _ := screens.Screen()
	if list := shownList(t, screen); list.GetFocus() != "row3" || list.FocusAt == nil || *list.FocusAt != 3 || list.GetSelected() != "row3" {
		t.Fatalf("the focus was %q at %v, selected %q", list.GetFocus(), list.FocusAt, list.GetSelected())
	}

	// A row ranked above the focus moves its place down by one.
	above := listShown(0, "arena", false, 0, 0)
	above.GetUi().GetChange().GetSet()[0].List.Selected = "row3"
	above.GetUi().GetChange().GetLists()[0].Set = []*ui.Row{{Key: "top", Rank: -1}}
	screens.Handle(above)
	screen, _ = screens.Screen()
	if list := shownList(t, screen); list.FocusAt == nil || *list.FocusAt != 4 {
		t.Fatalf("after a row arrived above, the focus was at %v", list.FocusAt)
	}

	// Once the row leaves, the focus echoes with no place and the selection
	// clears; the snapshot carries the focus.
	gone := listShown(0, "arena", false, 0, 0)
	gone.GetUi().GetChange().GetSet()[0].List.Selected = "row3"
	gone.GetUi().GetChange().GetLists()[0].Removed = []string{"row3"}
	screens.Handle(gone)
	screen, _ = screens.Screen()
	if list := shownList(t, screen); list.GetFocus() != "row3" || list.FocusAt != nil || list.GetSelected() != "" {
		t.Fatalf("after the row left, the focus was %q at %v, selected %q", list.GetFocus(), list.FocusAt, list.GetSelected())
	}
	copied := NewScreens(1)
	for _, event := range screens.Snapshot() {
		copied.Handle(event)
	}
	if got, _ := copied.Screen(); !got.EqualVT(screen) {
		t.Fatalf("the snapshot lost the focus: %v", shownList(t, got))
	}
}
