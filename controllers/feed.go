package controllers

import (
	"context"
	"errors"
	"io"

	"github.com/s4wave/spacewave/net/protocol"
)

// FeedProtocolID identifies the peer streams that carry the ControllerFeed
// service from a player's own client to their game server.
const FeedProtocolID = protocol.ID("modlock/controllers/1")

// ServeFeed takes the controller frames of guest, the peer the stream came
// over, for the slot its own game connection holds, and sends the mods' want
// for that slot as it changes, until the guest or the stream ends. An empty
// guest feeds the host's own player, so a server calls it only with the peer
// its transport authenticated.
func (h *Hub) ServeFeed(guest string, strm SRPCControllerFeed_FeedStream) error {
	// Bind the stream to the guest.
	feed := h.Open(guest)
	defer feed.Close()

	// Send the wants beside the frames.
	ctx, cancel := context.WithCancel(strm.Context())
	defer cancel()
	go func() {
		_ = feed.Serve(ctx, strm.Send)
		cancel()
	}()

	// Pass each frame to the slot until the guest stops.
	for {
		frame, err := strm.Recv()
		if err != nil {
			if errors.Is(err, io.EOF) || ctx.Err() != nil {
				return nil
			}
			return err
		}
		feed.Frame(frame)
	}
}

// FeedHost streams this PC's controllers to a game server for the player
// whose game this peer carries, reading them only while the server's mods
// watch, until ctx ends. A server that serves no feed, or one that drops it,
// leaves the player with no controllers and the game playing on.
func FeedHost(ctx context.Context, host SRPCControllerFeedClient) {
	// Open the stream; the server binds it to this peer's game connection.
	strm, err := host.Feed(ctx)
	if err != nil {
		return
	}
	defer strm.Close()

	// Follow the server's want, keeping only the latest epoch.
	wants := make(chan uint32, 1)
	go func() {
		defer close(wants)
		for {
			want, err := strm.Recv()
			if err != nil {
				return
			}
			select {
			case <-wants:
			default:
			}
			wants <- want.GetEpoch()
		}
	}()

	// Read the devices while wanted.
	_ = Serve(ctx, nil, wants, strm.Send)
}
