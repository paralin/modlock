package peernet

import (
	"context"
	"errors"

	"github.com/paralin/modlock/controllers"
	"github.com/s4wave/spacewave/net/peer"
)

// feedServer serves the hosted session's ControllerFeed, binding each stream
// to the guest whose peer link it came over.
type feedServer struct {
	// hub binds the streams.
	hub *controllers.Hub
}

// Feed takes the guest's controllers for the slot its own game connection
// holds, until the guest or the session ends.
func (s *feedServer) Feed(strm controllers.SRPCControllerFeed_FeedStream) error {
	guest := peerOf(strm.Context())
	if guest == "" {
		return errors.New("controllers come only over a guest's peer link")
	}
	return s.hub.ServeFeed(guest.String(), strm)
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
