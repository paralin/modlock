package peernet

import (
	"context"
	"maps"
	"net"
	"slices"
	"sync"

	"github.com/paralin/modlock/src/host_app/control"
	pb "github.com/paralin/modlock/src/wasm/controllers"
	"github.com/s4wave/spacewave/net/peer"
)

// controllerHub binds each controller stream on a hosting PC to the one
// player whose game connection it may feed, and passes that player's frames
// to the server and the server's wants back.
//
// A guest's stream feeds the slot whose client connects from one of the
// sockets the host bound to forward that guest's game stream, as the server
// reports in PlayerJoined. A frame names no player, so no guest can feed
// another's slot. The host's own player is the slot connecting from a
// loopback address that forwards no guest, which the daemon feeds from this
// PC's own devices.
type controllerHub struct {
	// events receives each player's frames, and the end of their stream.
	events func(*HostEvent)

	// mu guards the fields below.
	mu sync.Mutex
	// players maps each joined player's slot to the address their client
	// connects from.
	players map[int32]string
	// wants maps each watched slot to its epoch.
	wants map[int32]uint32
	// forwards maps each guest's peer to the addresses of the sockets that
	// forward its game streams, each counted by its open streams.
	forwards map[peer.ID]map[string]int
	// feeds holds the open controller streams.
	feeds map[*controllerFeed]struct{}
}

// controllerFeed is one controller stream: a guest's, or the host player's
// own when guest is empty.
type controllerFeed struct {
	// guest is the guest's peer, or empty for the host's own player.
	guest peer.ID
	// changed holds a token when the feed's want may have changed.
	changed chan struct{}
	// bound is the slot the feed's frames go to, or -1, guarded by the hub.
	bound int32
}

// newControllerHub returns a hub that passes frames to events.
func newControllerHub(events func(*HostEvent)) *controllerHub {
	return &controllerHub{
		events:   events,
		players:  make(map[int32]string),
		wants:    make(map[int32]uint32),
		forwards: make(map[peer.ID]map[string]int),
		feeds:    make(map[*controllerFeed]struct{}),
	}
}

// handle follows a server event: a join that gives a slot's address, a leave
// that frees it, and the set of watched players.
func (h *controllerHub) handle(event *control.HostEvent) {
	// Take the change.
	h.mu.Lock()
	defer h.mu.Unlock()
	switch {
	case event.GetJoined() != nil:
		joined := event.GetJoined()
		if joined.GetSteamId() == 0 || joined.GetAddress() == "" {
			delete(h.players, joined.GetSlot())
		} else {
			h.players[joined.GetSlot()] = joined.GetAddress()
		}
	case event.GetLeft() != nil:
		delete(h.players, event.GetLeft().GetSlot())
	case event.GetControllersWanted() != nil:
		clear(h.wants)
		for _, want := range event.GetControllersWanted().GetWants() {
			h.wants[want.GetPlayer()] = want.GetEpoch()
		}
	default:
		return
	}
	h.rebind()
}

// forward records that a socket at local forwards guest's game stream, and
// returns the call that forgets it once the stream ends.
func (h *controllerHub) forward(guest peer.ID, local *net.UDPAddr) func() {
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

// open starts a controller stream for guest, or for the host's own player
// when guest is empty.
func (h *controllerHub) open(guest peer.ID) *controllerFeed {
	// Add the feed, then bind it to its slot.
	feed := &controllerFeed{guest: guest, changed: make(chan struct{}, 1), bound: -1}
	h.mu.Lock()
	defer h.mu.Unlock()
	h.feeds[feed] = struct{}{}
	h.rebind()
	return feed
}

// close ends feed's stream, ending its player's stream at the server.
func (h *controllerHub) close(feed *controllerFeed) {
	h.mu.Lock()
	defer h.mu.Unlock()
	delete(h.feeds, feed)
	h.bind(feed, -1)
}

// want returns feed's slot, or -1, and the epoch the mods watch it at, zero
// when they do not.
func (h *controllerHub) want(feed *controllerFeed) (int32, uint32) {
	h.mu.Lock()
	defer h.mu.Unlock()
	if feed.bound < 0 {
		return -1, 0
	}
	return feed.bound, h.wants[feed.bound]
}

// frame passes a frame from feed to its slot's player while the mods watch
// them, and drops it otherwise.
func (h *controllerHub) frame(feed *controllerFeed, frame *pb.ControllerFrame) {
	// Drop a frame for no slot or an unwatched one.
	h.mu.Lock()
	defer h.mu.Unlock()
	if feed.bound < 0 || h.wants[feed.bound] == 0 {
		return
	}

	// Pass it to the slot's player.
	frames := &control.ControllerFrames{Player: feed.bound, Frames: []*pb.ControllerFrame{frame}}
	h.events(&HostEvent{Body: &HostEvent_Controllers{Controllers: frames}})
}

// rebind binds every feed to its slot after a change. Callers hold mu.
func (h *controllerHub) rebind() {
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
func (h *controllerHub) bind(feed *controllerFeed, slot int32) {
	if feed.bound == slot {
		return
	}
	if feed.bound >= 0 {
		ended := &control.ControllerFrames{Player: feed.bound, Ended: true}
		h.events(&HostEvent{Body: &HostEvent_Controllers{Controllers: ended}})
	}
	feed.bound = slot
}

// slotOf returns the slot feed may feed, or -1: for a guest, the lowest slot
// connecting through one of its sockets; for the host's own player, the
// lowest connecting from loopback through no guest's socket. Callers hold mu.
func (h *controllerHub) slotOf(feed *controllerFeed) int32 {
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
func (h *controllerHub) local(addr string) bool {
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

// serve sends feed's epoch to want each time it changes, starting with the
// current one, until ctx ends or want fails.
func (h *controllerHub) serve(ctx context.Context, feed *controllerFeed, want func(*pb.ControllerWant) error) error {
	sent := &pb.ControllerWant{Player: -2}
	for {
		slot, epoch := h.want(feed)
		if slot != sent.GetPlayer() || epoch != sent.GetEpoch() {
			sent = &pb.ControllerWant{Player: slot, Epoch: epoch}
			if err := want(sent); err != nil {
				return err
			}
		}
		select {
		case <-ctx.Done():
			return nil
		case <-feed.changed:
		}
	}
}

// peerKey is the context key of the peer a session call came from.
type peerKey struct{}

// withPeer returns ctx carrying the peer a session call comes from.
func withPeer(ctx context.Context, id peer.ID) context.Context {
	return context.WithValue(ctx, peerKey{}, id)
}

// peerOf returns the peer a session call came from, or empty.
func peerOf(ctx context.Context) peer.ID {
	id, _ := ctx.Value(peerKey{}).(peer.ID)
	return id
}
