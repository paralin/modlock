package controllers

import (
	"context"
	"net"
	"testing"
	"time"

	"github.com/aperturerobotics/starpc/srpc"
	pb "github.com/paralin/modlock/src/wasm/controllers"
)

// TestHubBindsOwnSlot tries to feed another player's slot: a guest
// whose game connects as slot 2 streams while slot 1 is watched, a peer with
// no game stream streams, and the host's own feed streams. Only the host's
// own player and the guest's own slot receive frames; the rest are dropped
// and told they are unwanted, and a stream that ends ends its slot.
func TestHubBindsOwnSlot(t *testing.T) {
	var events []*pb.ControllerFrames
	hub := NewHub(func(frames *pb.ControllerFrames) { events = append(events, frames) })
	want := func(wants ...*pb.ControllerWant) { hub.Wanted(&pb.ControllerWants{Wants: wants}) }
	frame := &pb.ControllerFrame{Epoch: 1, Full: true}

	// The host player connects from loopback, the guest through its
	// forward socket, and slot 1 alone is watched.
	guest, stranger := "guest", "stranger"
	release := hub.Forward(guest, &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: 40002})
	hub.Joined(1, "127.0.0.1:27005", 0)
	hub.Joined(2, "127.0.0.1:40002", 0)
	want(&pb.ControllerWant{Player: 1, Epoch: 7})
	own, theirs, none := hub.Open(""), hub.Open(guest), hub.Open(stranger)
	if slot, epoch := own.Want(); slot != 1 || epoch != 7 {
		t.Fatalf("host player bound to %d at %d", slot, epoch)
	}
	if slot, epoch := theirs.Want(); slot != 2 || epoch != 0 {
		t.Fatalf("guest bound to %d at %d", slot, epoch)
	}
	if slot, epoch := none.Want(); slot != -1 || epoch != 0 {
		t.Fatalf("stranger bound to %d at %d", slot, epoch)
	}

	// The guest's and stranger's frames reach no one; the host's own reach
	// slot 1.
	theirs.Frame(frame)
	none.Frame(frame)
	own.Frame(frame)
	if len(events) != 1 || events[0].GetPlayer() != 1 || len(events[0].GetFrames()) != 1 {
		t.Fatalf("events after the frames: %v", events)
	}

	// Once slot 2 is watched the guest feeds it, and only it.
	want(&pb.ControllerWant{Player: 1, Epoch: 7}, &pb.ControllerWant{Player: 2, Epoch: 8})
	theirs.Frame(frame)
	if last := events[len(events)-1]; last.GetPlayer() != 2 {
		t.Fatalf("guest frame went to %d", last.GetPlayer())
	}

	// The guest's game stream ending unbinds it and ends slot 2's stream.
	events = nil
	release()
	if slot, _ := theirs.Want(); slot != -1 || len(events) != 1 || !events[0].GetEnded() || events[0].GetPlayer() != 2 {
		t.Fatalf("after release: slot %d, events %v", slot, events)
	}
	theirs.Frame(frame)
	if len(events) != 1 {
		t.Fatalf("unbound guest fed: %v", events)
	}

	// Closing the host's feed ends slot 1's stream.
	own.Close()
	if last := events[len(events)-1]; !last.GetEnded() || last.GetPlayer() != 1 {
		t.Fatalf("after close: %v", events)
	}
}

// TestHubBindsVerifiedAccount tries to feed a directly connected player's
// slot: a guest the server verified as account 76 feeds the slot signed in to
// 76, a guest naming no account and one verified as another feed nothing,
// and dropping the member unbinds it and ends the slot's stream.
func TestHubBindsVerifiedAccount(t *testing.T) {
	var events []*pb.ControllerFrames
	hub := NewHub(func(frames *pb.ControllerFrames) { events = append(events, frames) })
	hub.Joined(4, "203.0.113.9:27005", 76)
	hub.Joined(5, "198.51.100.2:27005", 77)
	hub.Wanted(&pb.ControllerWants{Wants: []*pb.ControllerWant{{Player: 4, Epoch: 2}, {Player: 5, Epoch: 3}}})
	hub.Members(map[string]uint64{"guest": 76, "other": 78})
	theirs, stranger, other := hub.Open("guest"), hub.Open("stranger"), hub.Open("other")
	if slot, epoch := theirs.Want(); slot != 4 || epoch != 2 {
		t.Fatalf("member bound to %d at %d", slot, epoch)
	}
	for _, feed := range []*Feed{stranger, other} {
		if slot, _ := feed.Want(); slot != -1 {
			t.Fatalf("%s bound to %d", feed.guest, slot)
		}
		feed.Frame(&pb.ControllerFrame{Epoch: 3, Full: true})
	}
	if len(events) != 0 {
		t.Fatalf("frames from unbound feeds: %v", events)
	}

	// Dropping the member ends slot 4's stream.
	hub.Members(nil)
	if slot, _ := theirs.Want(); slot != -1 || len(events) != 1 || !events[0].GetEnded() || events[0].GetPlayer() != 4 {
		t.Fatalf("after the roster change: slot %d, events %v", slot, events)
	}
}

// TestHubServeSendsChanges sees Serve send the current want, then
// each change, and stop when its context ends.
func TestHubServeSendsChanges(t *testing.T) {
	hub := NewHub(func(*pb.ControllerFrames) {})
	hub.Joined(3, "loopback", 0)
	feed := hub.Open("")

	// Serve into a channel, which first sees slot 3 unwatched.
	ctx, cancel := context.WithCancel(context.Background())
	sent := make(chan *pb.ControllerWant, 8)
	done := make(chan error, 1)
	go func() {
		done <- feed.Serve(ctx, func(w *pb.ControllerWant) error { sent <- w; return nil })
	}()
	next := func() *pb.ControllerWant {
		select {
		case w := <-sent:
			return w
		case <-time.After(2 * time.Second):
			t.Fatal("no want sent")
			return nil
		}
	}
	if w := next(); w.GetPlayer() != 3 || w.GetEpoch() != 0 {
		t.Fatalf("first want: %v", w)
	}

	// A watch sends its epoch.
	hub.Wanted(&pb.ControllerWants{Wants: []*pb.ControllerWant{{Player: 3, Epoch: 4}}})
	if w := next(); w.GetPlayer() != 3 || w.GetEpoch() != 4 {
		t.Fatalf("watched want: %v", w)
	}

	// Ending the context ends serve.
	cancel()
	if err := <-done; err != nil {
		t.Fatal(err)
	}
}

// guestFeed serves the ControllerFeed for one guest, as a server does with
// the peer its transport authenticated.
type guestFeed struct {
	hub   *Hub
	guest string
}

// Feed binds the stream to the guest.
func (g guestFeed) Feed(strm SRPCControllerFeed_FeedStream) error {
	return g.hub.ServeFeed(g.guest, strm)
}

// TestFeedCarriesAGuestsDevices runs FeedHost against a server's ServeFeed
// with the virtual devices: nothing reaches the guest's slot until a mod
// watches it, and then the first frame of the watch's epoch lists the
// virtual gamepad.
func TestFeedCarriesAGuestsDevices(t *testing.T) {
	// A server whose slot 2 connects through the guest's forwarding socket.
	testSDL(t)
	t.Setenv(VirtualEnv, "1")
	frames := make(chan *pb.ControllerFrames, 256)
	hub := NewHub(func(f *pb.ControllerFrames) {
		select {
		case frames <- f:
		default:
		}
	})
	defer hub.Forward("guest", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: 40002})()
	hub.Joined(2, "127.0.0.1:40002", 0)
	mux := srpc.NewMux()
	if err := SRPCRegisterControllerFeed(mux, guestFeed{hub: hub, guest: "guest"}); err != nil {
		t.Fatal(err)
	}
	client := NewSRPCControllerFeedClient(srpc.NewClient(srpc.NewServerPipe(srpc.NewServer(mux))))

	// The guest's client feeds while unwatched, and nothing arrives.
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	go FeedHost(ctx, client)
	select {
	case f := <-frames:
		t.Fatalf("frames before a watch: %v", f)
	case <-time.After(300 * time.Millisecond):
	}

	// A watch of slot 2 brings its epoch's full frame with the gamepad.
	hub.Wanted(&pb.ControllerWants{Wants: []*pb.ControllerWant{{Player: 2, Epoch: 5}}})
	for {
		select {
		case f := <-frames:
			first := f.GetFrames()[0]
			if f.GetPlayer() != 2 || first.GetEpoch() != 5 || !first.GetFull() {
				continue
			}
			for _, info := range first.GetAdded() {
				if info.GetName() == "Modlock virtual gamepad" {
					return
				}
			}
			t.Fatalf("the full frame lists no virtual gamepad: %v", first)
		case <-ctx.Done():
			t.Fatal("no full frame for the watch")
		}
	}
}
