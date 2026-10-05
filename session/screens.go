package session

import (
	"maps"
	"net"
	"net/http"
	"slices"
	"strings"
	"sync"
	"time"

	"github.com/paralin/modlock/proto/modlock/control"
	"github.com/paralin/modlock/proto/modlock/ui"
)

// RelayAgent marks the game's own HTML panels in their User-Agent. Requiring
// it keeps the relay page, opened in an ordinary browser, from reading the
// player's screen. A Steam beta client names its
// branch after the marker, as "Source2 HTML [Steam Beta Update]/".
const RelayAgent = "Source2 HTML"

// frameGap is the least time between two frames on one stream. The relay page
// acknowledges each frame before it reads the next, so a mod that changes its
// interface every server frame would otherwise queue frames in the socket
// faster than the game draws them; the stream sends the newest screen instead.
const frameGap = 50 * time.Millisecond

// Screens keeps what the mods show the local player and serves it to the
// player's game on loopback. Deadlock loads only https pages into its HTML
// panels, so the game's renderer reads through a relay page from another
// origin: the page streams the screen from GET /panorama/stream, one JSON
// ui.Screen per line. The game presses buttons with a console command to the
// server, not through this page.
type Screens struct {
	// account is the local player's Steam ID, or zero to take the first
	// person who joins.
	account uint64
	// origin is the relay page's origin.
	origin string

	// mu guards the fields below.
	mu sync.Mutex
	// slot is the local player's slot, or -1 while they are not in the server.
	slot int32
	// trees holds each mod's nodes by id, per slot.
	trees map[int32]map[string]map[string]*ui.Node
	// changed is closed and replaced whenever the local player's screen
	// changes.
	changed chan struct{}
}

// NewScreens returns screens for the player with the Steam ID account, read
// through the relay page from origin.
func NewScreens(account uint64, origin string) *Screens {
	return &Screens{
		account: account,
		origin:  origin,
		slot:    -1,
		trees:   make(map[int32]map[string]map[string]*ui.Node),
		changed: make(chan struct{}),
	}
}

// Handle follows one host event: a join that binds the local player's slot,
// a leave that frees it, or a mod's change to a player's interface.
func (s *Screens) Handle(event *control.HostEvent) {
	s.mu.Lock()
	defer s.mu.Unlock()
	switch {
	case event.GetJoined() != nil:
		joined := event.GetJoined()
		person := joined.GetSteamId()
		switch {
		case person != 0 && (person == s.account || s.account == 0 && s.slot < 0):
			s.slot = joined.GetSlot()
		case joined.GetSlot() == s.slot:
			// Someone else took the slot, so the local player left.
			s.slot = -1
		default:
			return
		}
		s.notify()
	case event.GetLeft() != nil:
		if event.GetLeft().GetSlot() != s.slot {
			return
		}
		s.slot = -1
		s.notify()
	case event.GetUi() != nil:
		update := event.GetUi()
		s.apply(update.GetSlot(), update.GetMod(), update.GetChange())
		if update.GetSlot() == s.slot {
			s.notify()
		}
	}
}

// apply applies change to mod's tree for slot.
func (s *Screens) apply(slot int32, mod string, change *ui.Change) {
	// Find the player's trees, and start the mod's tree over on a reset.
	mods := s.trees[slot]
	if mods == nil {
		mods = make(map[string]map[string]*ui.Node)
		s.trees[slot] = mods
	}
	nodes := mods[mod]
	if nodes == nil || change.GetReset_() {
		nodes = make(map[string]*ui.Node)
	}

	// Set and remove the changed nodes.
	for _, node := range change.GetSet() {
		nodes[node.GetId()] = node
	}
	for _, id := range change.GetRemoved() {
		delete(nodes, id)
	}

	// Keep only a tree that still has nodes.
	if len(nodes) == 0 {
		delete(mods, mod)
		return
	}
	mods[mod] = nodes
}

// notify wakes every stream. Callers hold mu.
func (s *Screens) notify() {
	close(s.changed)
	s.changed = make(chan struct{})
}

// Screen returns the local player's screen and a channel that closes when it
// changes.
func (s *Screens) Screen() (*ui.Screen, <-chan struct{}) {
	// Collect the local player's trees in a stable order.
	s.mu.Lock()
	defer s.mu.Unlock()
	screen := &ui.Screen{}
	mods := s.trees[s.slot]
	for _, mod := range slices.Sorted(maps.Keys(mods)) {
		tree := &ui.Tree{Mod: mod}
		for _, id := range slices.Sorted(maps.Keys(mods[mod])) {
			tree.Nodes = append(tree.Nodes, mods[mod][id])
		}
		screen.Trees = append(screen.Trees, tree)
	}
	return screen, s.changed
}

// ServeHTTP serves the screen stream to the relay page.
func (s *Screens) ServeHTTP(w http.ResponseWriter, request *http.Request) {
	if !s.admit(w, request) {
		return
	}
	switch {
	case request.Method == http.MethodGet && request.URL.Path == "/panorama/stream":
		s.stream(w, request)
	default:
		http.NotFound(w, request)
	}
}

// admit refuses requests from anything but the game's relay page or a
// same-origin page on loopback, and answers the relay's preflight. It reports
// whether the request needs more.
func (s *Screens) admit(w http.ResponseWriter, request *http.Request) bool {
	// Admit only loopback host names, which keeps DNS rebinding out, and
	// only the relay or a same-origin page.
	host, _, err := net.SplitHostPort(request.Host)
	local := err == nil && (host == "127.0.0.1" || strings.EqualFold(host, "localhost"))
	origin := request.Header.Get("Origin")
	relay := origin != "" && origin == s.origin && strings.Contains(request.UserAgent(), RelayAgent)
	sameOrigin := origin == "" || origin == "http://"+request.Host
	if !local || !relay && !sameOrigin {
		http.Error(w, "Forbidden", http.StatusForbidden)
		return false
	}

	// Answer the relay under CORS and Private Network Access.
	if relay {
		// The relay is a public https page reaching a private address, so the
		// browser asks first under CORS and Private Network Access.
		w.Header().Set("Access-Control-Allow-Origin", s.origin)
		w.Header().Set("Access-Control-Allow-Private-Network", "true")
		w.Header().Set("Vary", "Origin")
		if request.Method == http.MethodOptions {
			w.Header().Set("Access-Control-Allow-Methods", "GET")
			w.WriteHeader(http.StatusNoContent)
			return false
		}
	}
	w.Header().Set("Cache-Control", "no-store")
	return true
}

// stream writes the local player's screen, then each changed screen, until
// the page goes away.
func (s *Screens) stream(w http.ResponseWriter, request *http.Request) {
	controller := http.NewResponseController(w)
	w.Header().Set("Content-Type", "application/x-ndjson")
	for {
		// Send the current screen as one line.
		screen, changed := s.Screen()
		line, err := screen.MarshalJSON()
		if err != nil {
			return
		}
		if _, err := w.Write(append(line, '\n')); err != nil {
			return
		}
		if controller.Flush() != nil {
			return
		}

		// Wait for a change, then let the page draw before the next frame.
		select {
		case <-changed:
		case <-request.Context().Done():
			return
		}
		select {
		case <-time.After(frameGap):
		case <-request.Context().Done():
			return
		}
	}
}

var _ http.Handler = (*Screens)(nil)
