package peernet

import (
	"context"
	"net"
	"testing"
	"time"

	"github.com/paralin/modlock/src/host_app/control"
	pb "github.com/paralin/modlock/src/wasm/controllers"
	"github.com/s4wave/spacewave/net/peer"
)

// TestControllerHubBindsOwnSlot tries to feed another player's slot: a guest
// whose game connects as slot 2 streams while slot 1 is watched, a peer with
// no game stream streams, and the host's own feed streams. Only the host's
// own player and the guest's own slot receive frames; the rest are dropped
// and told they are unwanted, and a stream that ends ends its slot.
func TestControllerHubBindsOwnSlot(t *testing.T) {
	var events []*control.ControllerFrames
	hub := newControllerHub(func(event *HostEvent) { events = append(events, event.GetControllers()) })
	join := func(slot int32, addr string) {
		hub.handle(&control.HostEvent{Body: &control.HostEvent_Joined{Joined: &control.PlayerJoined{Slot: slot, SteamId: uint64(100 + slot), Address: addr}}})
	}
	want := func(wants ...*pb.ControllerWant) {
		hub.handle(&control.HostEvent{Body: &control.HostEvent_ControllersWanted{ControllersWanted: &pb.ControllerWants{Wants: wants}}})
	}
	frame := &pb.ControllerFrame{Epoch: 1, Full: true}

	// The host player connects from loopback, the guest through its
	// forward socket, and slot 1 alone is watched.
	guest, stranger := peer.ID("guest"), peer.ID("stranger")
	release := hub.forward(guest, &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: 40002})
	join(1, "127.0.0.1:27005")
	join(2, "127.0.0.1:40002")
	want(&pb.ControllerWant{Player: 1, Epoch: 7})
	own, theirs, none := hub.open(""), hub.open(guest), hub.open(stranger)
	if slot, epoch := hub.want(own); slot != 1 || epoch != 7 {
		t.Fatalf("host player bound to %d at %d", slot, epoch)
	}
	if slot, epoch := hub.want(theirs); slot != 2 || epoch != 0 {
		t.Fatalf("guest bound to %d at %d", slot, epoch)
	}
	if slot, epoch := hub.want(none); slot != -1 || epoch != 0 {
		t.Fatalf("stranger bound to %d at %d", slot, epoch)
	}

	// The guest's and stranger's frames reach no one; the host's own reach
	// slot 1.
	hub.frame(theirs, frame)
	hub.frame(none, frame)
	hub.frame(own, frame)
	if len(events) != 1 || events[0].GetPlayer() != 1 || len(events[0].GetFrames()) != 1 {
		t.Fatalf("events after the frames: %v", events)
	}

	// Once slot 2 is watched the guest feeds it, and only it.
	want(&pb.ControllerWant{Player: 1, Epoch: 7}, &pb.ControllerWant{Player: 2, Epoch: 8})
	hub.frame(theirs, frame)
	if last := events[len(events)-1]; last.GetPlayer() != 2 {
		t.Fatalf("guest frame went to %d", last.GetPlayer())
	}

	// The guest's game stream ending unbinds it and ends slot 2's stream.
	events = nil
	release()
	if slot, _ := hub.want(theirs); slot != -1 || len(events) != 1 || !events[0].GetEnded() || events[0].GetPlayer() != 2 {
		t.Fatalf("after release: slot %d, events %v", slot, events)
	}
	hub.frame(theirs, frame)
	if len(events) != 1 {
		t.Fatalf("unbound guest fed: %v", events)
	}

	// Closing the host's feed ends slot 1's stream.
	hub.close(own)
	if last := events[len(events)-1]; !last.GetEnded() || last.GetPlayer() != 1 {
		t.Fatalf("after close: %v", events)
	}
}

// TestControllerHubServeSendsChanges sees serve send the current want, then
// each change, and stop when its context ends.
func TestControllerHubServeSendsChanges(t *testing.T) {
	hub := newControllerHub(func(*HostEvent) {})
	hub.handle(&control.HostEvent{Body: &control.HostEvent_Joined{Joined: &control.PlayerJoined{Slot: 3, SteamId: 1, Address: "loopback"}}})
	feed := hub.open("")

	// Serve into a channel, which first sees slot 3 unwatched.
	ctx, cancel := context.WithCancel(context.Background())
	sent := make(chan *pb.ControllerWant, 8)
	done := make(chan error, 1)
	go func() {
		done <- hub.serve(ctx, feed, func(w *pb.ControllerWant) error { sent <- w; return nil })
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
	hub.handle(&control.HostEvent{Body: &control.HostEvent_ControllersWanted{ControllersWanted: &pb.ControllerWants{Wants: []*pb.ControllerWant{{Player: 3, Epoch: 4}}}}})
	if w := next(); w.GetPlayer() != 3 || w.GetEpoch() != 4 {
		t.Fatalf("watched want: %v", w)
	}

	// Ending the context ends serve.
	cancel()
	if err := <-done; err != nil {
		t.Fatal(err)
	}
}
