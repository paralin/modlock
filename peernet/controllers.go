package peernet

import (
	"context"

	"github.com/paralin/modlock/controllers"
	"github.com/paralin/modlock/src/host_app/control"
	"github.com/s4wave/spacewave/net/peer"
)

// followControllers passes the server events that bind controller streams to
// hub: a join that gives a slot's address, a leave that frees it, and the set
// of watched players. A join with no Steam ID is a bot, whose slot no stream
// may feed.
func followControllers(hub *controllers.Hub, event *control.HostEvent) {
	switch {
	case event.GetJoined() != nil:
		joined := event.GetJoined()
		address := joined.GetAddress()
		if joined.GetSteamId() == 0 {
			address = ""
		}
		hub.Joined(joined.GetSlot(), address)
	case event.GetLeft() != nil:
		hub.Left(event.GetLeft().GetSlot())
	case event.GetControllersWanted() != nil:
		hub.Wanted(event.GetControllersWanted())
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
