package session

import (
	"maps"
	"math/rand/v2"
	"slices"
	"sync"

	"github.com/paralin/modlock/src/host_app/control"
	"github.com/paralin/modlock/src/wasm/ui"
)

// Screens keeps what the mods show each player on a server, following the
// host's events. It serves the local player's screen to a Relay, and any
// player's screen to a host that streams it to a guest.
type Screens struct {
	// account is the local player's Steam ID, or zero to take the first
	// person who joins.
	account uint64

	// mu guards the fields below.
	mu sync.Mutex
	// slot is the local player's slot, or -1 while they are not in the server.
	slot int32
	// players holds each joined person's join, by slot.
	players map[int32]*control.PlayerJoined
	// trees holds each mod's tree, per slot.
	trees map[int32]trees
	// paused is whether the game is paused, which every screen carries.
	paused bool
	// changed is closed and replaced whenever any player's screen changes.
	changed chan struct{}
}

// NewScreens returns screens whose local player has the Steam ID account.
func NewScreens(account uint64) *Screens {
	return &Screens{
		account: account,
		slot:    -1,
		players: make(map[int32]*control.PlayerJoined),
		trees:   make(map[int32]trees),
		changed: make(chan struct{}),
	}
}

// Handle follows one host event: a join that binds a person's slot, a leave
// that frees it, a mod's change to a player's interface, the window of a
// list's rows that player's renderer asks for, or the game pausing or
// resuming.
func (s *Screens) Handle(event *control.HostEvent) {
	s.mu.Lock()
	defer s.mu.Unlock()
	switch {
	case event.GetJoined() != nil:
		s.join(event.GetJoined())
	case event.GetLeft() != nil:
		slot := event.GetLeft().GetSlot()
		delete(s.players, slot)
		if slot == s.slot {
			s.slot = -1
		}
	case event.GetUi() != nil:
		update := event.GetUi()
		mods := s.trees[update.GetSlot()]
		if mods == nil {
			mods = make(trees)
			s.trees[update.GetSlot()] = mods
		}
		mods.apply(update.GetMod(), update.GetChange())
	case event.GetUiWindow() != nil:
		update := event.GetUiWindow()
		if !s.trees[update.GetSlot()].window(update.GetMod(), update.GetWindow()) {
			return
		}
	case event.GetPaused() != nil:
		paused := event.GetPaused().GetPaused()
		if paused == s.paused {
			return
		}
		s.paused = paused
	default:
		return
	}
	s.notify()
}

// join binds a person to their slot, and the local player to theirs. Callers
// hold mu.
func (s *Screens) join(joined *control.PlayerJoined) {
	// Record the person, who replaces whoever held the slot; a bot is no one.
	person, slot := joined.GetSteamId(), joined.GetSlot()
	if person != 0 {
		s.players[slot] = joined
	} else {
		delete(s.players, slot)
	}

	// Follow the local player, who left when someone else took their slot.
	switch {
	case person != 0 && (person == s.account || s.account == 0 && s.slot < 0):
		s.slot = slot
	case slot == s.slot:
		s.slot = -1
	}
}

// notify wakes every stream. Callers hold mu.
func (s *Screens) notify() {
	close(s.changed)
	s.changed = make(chan struct{})
}

// Screen returns the local player's screen and a channel that closes when it
// may have changed.
func (s *Screens) Screen() (*ui.Screen, <-chan struct{}) {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.pause(s.trees[s.slot].screen(true)), s.changed
}

// Player returns the screen of the person with the Steam ID account, whether
// they are in the server, and a channel that closes when either may have
// changed. The screen is empty while they are not in the server.
func (s *Screens) Player(account uint64) (*ui.Screen, bool, <-chan struct{}) {
	s.mu.Lock()
	defer s.mu.Unlock()
	for slot, joined := range s.players {
		if joined.GetSteamId() == account {
			return s.pause(s.trees[slot].screen(true)), true, s.changed
		}
	}
	return s.pause(&ui.Screen{}), false, s.changed
}

// pause marks screen with whether the game is paused. Callers hold mu.
func (s *Screens) pause(screen *ui.Screen) *ui.Screen {
	screen.Paused = s.paused
	return screen
}

// Snapshot returns the events that bring new Screens to this one's state:
// each person's join, then each tree as a reset change with every list's
// rows, then each list's window, then the pause while the game is paused.
func (s *Screens) Snapshot() []*control.HostEvent {
	// Join each person, in slot order.
	s.mu.Lock()
	defer s.mu.Unlock()
	var events []*control.HostEvent
	for _, slot := range slices.Sorted(maps.Keys(s.players)) {
		events = append(events, &control.HostEvent{Body: &control.HostEvent_Joined{Joined: s.players[slot]}})
	}

	// Send every tree whole, and then the windows its lists show.
	for _, slot := range slices.Sorted(maps.Keys(s.trees)) {
		mods := s.trees[slot]
		var windows []*control.HostEvent
		for _, tree := range mods.screen(false).GetTrees() {
			lists := mods[tree.GetMod()].lists
			change := &ui.Change{Reset_: true, Set: tree.GetNodes(), Generation: tree.GetGeneration()}
			for _, node := range slices.Sorted(maps.Keys(lists)) {
				list := lists[node]
				change.Lists = append(change.Lists, list.reset(node))
				window := &ui.ListWindow{Node: node, First: list.first, Count: list.count, Focus: list.focus}
				update := &control.UiWindow{Mod: tree.GetMod(), Slot: slot, Window: window}
				windows = append(windows, &control.HostEvent{Body: &control.HostEvent_UiWindow{UiWindow: update}})
			}
			update := &control.UiChanged{Mod: tree.GetMod(), Slot: slot, Change: change}
			events = append(events, &control.HostEvent{Body: &control.HostEvent_Ui{Ui: update}})
		}
		events = append(events, windows...)
	}
	if s.paused {
		paused := &control.GamePaused{Paused: true}
		events = append(events, &control.HostEvent{Body: &control.HostEvent_Paused{Paused: paused}})
	}
	return events
}

// trees holds one player's mod trees, retaining cleared lifetimes so the next
// redraw cannot reuse a layout from before the clear.
type trees map[string]*tree

// tree keeps a mod's current nodes and the lifetime of their panels.
type tree struct {
	// nodes holds the current nodes by id, empty while the mod hides its UI.
	nodes map[string]*ui.Node
	// lists holds the rows of each list node, by id. A list node that has
	// none yet shows no rows.
	lists map[string]*rowList
	// generation changes on a clear and travels with subsequent redraws.
	generation uint64
}

// apply applies change to mod's tree.
func (t trees) apply(mod string, change *ui.Change) {
	// Start the mod's tree over on a reset.
	current := t[mod]
	if current == nil {
		current = &tree{generation: newGeneration()}
		t[mod] = current
	}
	if change.GetGeneration() != 0 {
		current.generation = change.GetGeneration()
	} else if change.GetReset_() && len(change.GetSet()) == 0 {
		current.generation++
	}
	if current.nodes == nil || change.GetReset_() {
		current.nodes = make(map[string]*ui.Node)
	}

	// Set and remove the changed nodes.
	for _, node := range change.GetSet() {
		current.nodes[node.GetId()] = node
	}
	for _, id := range change.GetRemoved() {
		delete(current.nodes, id)
	}

	// Change the lists' rows. A reset drops every row but keeps the windows
	// the renderer asked for, and a list whose node is gone drops its rows.
	if change.GetReset_() {
		for _, list := range current.lists {
			list.clear()
		}
	}
	for _, rows := range change.GetLists() {
		if list := current.list(rows.GetNode()); list != nil {
			list.apply(rows)
		}
	}
	for id := range current.lists {
		if current.nodes[id].GetKind() != ui.Kind_KIND_LIST {
			delete(current.lists, id)
		}
	}
}

// list returns the rows of the list node id, or nil when id is not a list.
func (t *tree) list(id string) *rowList {
	if t.nodes[id].GetKind() != ui.Kind_KIND_LIST {
		return nil
	}
	if t.lists == nil {
		t.lists = make(map[string]*rowList)
	}
	list := t.lists[id]
	if list == nil {
		list = newRowList()
		t.lists[id] = list
	}
	return list
}

// window sets the window of mod's list node, and reports whether mod's tree
// has that list.
func (t trees) window(mod string, window *ui.ListWindow) bool {
	current := t[mod]
	if current == nil {
		return false
	}
	list := current.list(window.GetNode())
	if list == nil {
		return false
	}
	list.window(window)
	return true
}

// newGeneration gives a new screen owner a nonzero lifetime that does not
// restart at the same counter after its process or session restarts.
func newGeneration() uint64 {
	for {
		if generation := rand.Uint64(); generation != 0 {
			return generation
		}
	}
}

// screen returns the trees as a screen, in a stable order. With fill, which
// the owner of the rows sets, each list node carries its window of rows;
// otherwise the nodes are as they arrived, as a Mirror's carry the rows its
// owner filled. The nodes are shared, so callers do not change them.
func (t trees) screen(fill bool) *ui.Screen {
	screen := &ui.Screen{}
	for _, mod := range slices.Sorted(maps.Keys(t)) {
		current := t[mod]
		if len(current.nodes) == 0 {
			continue
		}
		tree := &ui.Tree{Mod: mod, Generation: current.generation}
		for _, id := range slices.Sorted(maps.Keys(current.nodes)) {
			node := current.nodes[id]
			if fill && node.GetKind() == ui.Kind_KIND_LIST {
				list := current.lists[id]
				if list == nil {
					list = newRowList()
				}
				node = list.fill(node)
			}
			tree.Nodes = append(tree.Nodes, node)
		}
		screen.Trees = append(screen.Trees, tree)
	}
	return screen
}
