package controllers

import (
	"context"
	"maps"
	"net"
	"slices"
	"sync"

	"github.com/paralin/modlock/src/host_app/control"
	pb "github.com/paralin/modlock/src/wasm/controllers"
)

// Hub binds each controller stream on a game server to the one player whose
// game connection it may feed, passes that player's frames to the server, and
// tells each stream the epoch the server's mods watch its player at.
//
// A guest's stream feeds the slot whose client connects from one of the
// sockets the server bound to forward that guest's game stream, as the
// server reports when the player joins. A frame names no player, so no guest
// can feed another's slot. The host's own player, fed by a stream with no
// guest, is the slot connecting from a loopback address that forwards no
// guest.
//
// The program that runs the server owns the Hub: it reports joins, leaves and
// the watched players from the server, each forwarding socket from its game
// links, and opens a Feed for each controller stream it accepts.
type Hub struct {
	// send receives each player's frames, and the end of their stream.
	send func(*control.ControllerFrames)

	// mu guards the fields below.
	mu sync.Mutex
	// players maps each joined player's slot to the address their client
	// connects from.
	players map[int32]string
	// wants maps each watched slot to its epoch.
	wants map[int32]uint32
	// forwards maps each guest to the addresses of the sockets that forward
	// its game streams, each counted by its open streams.
	forwards map[string]map[string]int
	// feeds holds the open controller streams.
	feeds map[*Feed]struct{}
}

// Feed is one controller stream: a guest's, or the host player's own when
// its guest is empty.
type Feed struct {
	// hub binds the feed.
	hub *Hub
	// guest identifies the guest's peer, or is empty for the host's own player.
	guest string
	// changed holds a token when the feed's want may have changed.
	changed chan struct{}
	// bound is the slot the feed's frames go to, or -1, guarded by the hub.
	bound int32
}

// NewHub returns a hub that passes each bound player's frames, and the end
// of each player's stream, to send. send runs under the hub's lock, so it
// must not call the hub.
func NewHub(send func(*control.ControllerFrames)) *Hub {
	return &Hub{
		send:     send,
		players:  make(map[int32]string),
		wants:    make(map[int32]uint32),
		forwards: make(map[string]map[string]int),
		feeds:    make(map[*Feed]struct{}),
	}
}

// Joined records that the client in slot connects from address, its
// ip:port or the engine's "loopback". An empty address forgets the slot, as
// for a bot.
func (h *Hub) Joined(slot int32, address string) {
	h.mu.Lock()
	defer h.mu.Unlock()
	if address == "" {
		delete(h.players, slot)
	} else {
		h.players[slot] = address
	}
	h.rebind()
}

// Left forgets the player in slot.
func (h *Hub) Left(slot int32) {
	h.mu.Lock()
	defer h.mu.Unlock()
	delete(h.players, slot)
	h.rebind()
}

// Wanted replaces the watched players with wants, the server's latest list.
func (h *Hub) Wanted(wants *pb.ControllerWants) {
	// Take the list, then tell each feed.
	h.mu.Lock()
	defer h.mu.Unlock()
	clear(h.wants)
	for _, want := range wants.GetWants() {
		h.wants[want.GetPlayer()] = want.GetEpoch()
	}
	h.rebind()
}

// Forward records that a socket at local forwards guest's game stream, and
// returns the call that forgets it once the stream ends.
func (h *Hub) Forward(guest string, local *net.UDPAddr) func() {
	// Count the socket under its guest.
	addr := local.String()
	h.mu.Lock()
	defer h.mu.Unlock()
	if h.forwards[guest] == nil {
		h.forwards[guest] = make(map[string]int)
	}
	h.forwards[guest][addr]++
	h.rebind()

	// Forget it once its stream ends.
	return func() {
		// Drop the socket's count, and the guest once it has none.
		h.mu.Lock()
		defer h.mu.Unlock()
		if h.forwards[guest][addr]--; h.forwards[guest][addr] == 0 {
			delete(h.forwards[guest], addr)
		}
		if len(h.forwards[guest]) == 0 {
			delete(h.forwards, guest)
		}
		h.rebind()
	}
}

// Open starts a controller stream for guest, or for the host's own player
// when guest is empty. The caller closes it when the stream ends.
func (h *Hub) Open(guest string) *Feed {
	// Add the feed, then bind it to its slot.
	feed := &Feed{hub: h, guest: guest, changed: make(chan struct{}, 1), bound: -1}
	h.mu.Lock()
	defer h.mu.Unlock()
	h.feeds[feed] = struct{}{}
	h.rebind()
	return feed
}

// Close ends the feed's stream, ending its player's stream at the server.
func (f *Feed) Close() {
	// Drop the feed, then end its slot's stream.
	h := f.hub
	h.mu.Lock()
	defer h.mu.Unlock()
	delete(h.feeds, f)
	h.bind(f, -1)
}

// Want returns the feed's slot, or -1, and the epoch the mods watch it at,
// zero when they do not.
func (f *Feed) Want() (int32, uint32) {
	// Read the slot and its epoch together.
	h := f.hub
	h.mu.Lock()
	defer h.mu.Unlock()
	if f.bound < 0 {
		return -1, 0
	}
	return f.bound, h.wants[f.bound]
}

// Frame passes a frame from the feed to its slot's player while the mods
// watch them, and drops it otherwise.
func (f *Feed) Frame(frame *pb.ControllerFrame) {
	// Drop a frame for no slot or an unwatched one.
	h := f.hub
	h.mu.Lock()
	defer h.mu.Unlock()
	if f.bound < 0 || h.wants[f.bound] == 0 {
		return
	}

	// Pass it to the slot's player.
	h.send(&control.ControllerFrames{Player: f.bound, Frames: []*pb.ControllerFrame{frame}})
}

// Serve sends the feed's want to want each time it changes, starting with
// the current one, until ctx ends or want fails.
func (f *Feed) Serve(ctx context.Context, want func(*pb.ControllerWant) error) error {
	sent := &pb.ControllerWant{Player: -2}
	for {
		slot, epoch := f.Want()
		if slot != sent.GetPlayer() || epoch != sent.GetEpoch() {
			sent = &pb.ControllerWant{Player: slot, Epoch: epoch}
			if err := want(sent); err != nil {
				return err
			}
		}
		select {
		case <-ctx.Done():
			return nil
		case <-f.changed:
		}
	}
}

// rebind binds every feed to its slot after a change. Callers hold mu.
func (h *Hub) rebind() {
	for feed := range h.feeds {
		h.bind(feed, h.slotOf(feed))
		select {
		case feed.changed <- struct{}{}:
		default:
		}
	}
}

// bind moves feed to slot, ending the stream of the slot it fed before.
// Callers hold mu.
func (h *Hub) bind(feed *Feed, slot int32) {
	if feed.bound == slot {
		return
	}
	if feed.bound >= 0 {
		h.send(&control.ControllerFrames{Player: feed.bound, Ended: true})
	}
	feed.bound = slot
}

// slotOf returns the slot feed may feed, or -1: for a guest, the lowest slot
// connecting through one of its sockets; for the host's own player, the
// lowest connecting from loopback through no guest's socket. Callers hold mu.
func (h *Hub) slotOf(feed *Feed) int32 {
	for _, slot := range slices.Sorted(maps.Keys(h.players)) {
		addr := h.players[slot]
		if feed.guest != "" {
			if h.forwards[feed.guest][addr] > 0 {
				return slot
			}
			continue
		}
		if h.local(addr) {
			return slot
		}
	}
	return -1
}

// local reports whether addr is a loopback address that forwards no guest,
// including the engine's "loopback" for a client in its own process. Callers
// hold mu.
func (h *Hub) local(addr string) bool {
	// Take a loopback address only.
	if addr == "loopback" {
		return true
	}
	ap, err := net.ResolveUDPAddr("udp", addr)
	if err != nil || !ap.IP.IsLoopback() {
		return false
	}

	// Refuse an address one of the guests' sockets connects from.
	for _, sockets := range h.forwards {
		if sockets[addr] > 0 {
			return false
		}
	}
	return true
}
