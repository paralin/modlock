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
	// players holds the Steam ID of each joined person, by slot.
	players map[int32]uint64
	// trees holds each mod's tree, per slot.
	trees map[int32]trees
	// changed is closed and replaced whenever any player's screen changes.
	changed chan struct{}
}

// NewScreens returns screens whose local player has the Steam ID account.
func NewScreens(account uint64) *Screens {
	return &Screens{
		account: account,
		slot:    -1,
		players: make(map[int32]uint64),
		trees:   make(map[int32]trees),
		changed: make(chan struct{}),
	}
}

// Handle follows one host event: a join that binds a person's slot, a leave
// that frees it, or a mod's change to a player's interface.
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
		s.players[slot] = person
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
	return s.trees[s.slot].screen(), s.changed
}

// Player returns the screen of the person with the Steam ID account, whether
// they are in the server, and a channel that closes when either may have
// changed. The screen is empty while they are not in the server.
func (s *Screens) Player(account uint64) (*ui.Screen, bool, <-chan struct{}) {
	s.mu.Lock()
	defer s.mu.Unlock()
	for slot, person := range s.players {
		if person == account {
			return s.trees[slot].screen(), true, s.changed
		}
	}
	return &ui.Screen{}, false, s.changed
}

// Snapshot returns the events that bring new Screens to this one's state:
// each person's join, then each tree as a reset change.
func (s *Screens) Snapshot() []*control.HostEvent {
	// Join each person, in slot order.
	s.mu.Lock()
	defer s.mu.Unlock()
	var events []*control.HostEvent
	for _, slot := range slices.Sorted(maps.Keys(s.players)) {
		joined := &control.PlayerJoined{Slot: slot, SteamId: s.players[slot]}
		events = append(events, &control.HostEvent{Body: &control.HostEvent_Joined{Joined: joined}})
	}

	// Send every tree whole.
	for _, slot := range slices.Sorted(maps.Keys(s.trees)) {
		for _, tree := range s.trees[slot].screen().GetTrees() {
			change := &ui.Change{Reset_: true, Set: tree.GetNodes(), Generation: tree.GetGeneration()}
			update := &control.UiChanged{Mod: tree.GetMod(), Slot: slot, Change: change}
			events = append(events, &control.HostEvent{Body: &control.HostEvent_Ui{Ui: update}})
		}
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

// screen returns the trees as a screen, in a stable order. The nodes are
// shared, so callers do not change them.
func (t trees) screen() *ui.Screen {
	screen := &ui.Screen{}
	for _, mod := range slices.Sorted(maps.Keys(t)) {
		current := t[mod]
		if len(current.nodes) == 0 {
			continue
		}
		tree := &ui.Tree{Mod: mod, Generation: current.generation}
		for _, id := range slices.Sorted(maps.Keys(current.nodes)) {
			tree.Nodes = append(tree.Nodes, current.nodes[id])
		}
		screen.Trees = append(screen.Trees, tree)
	}
	return screen
}
