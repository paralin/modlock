package gamelink

import (
	"context"
	"net"

	"github.com/s4wave/spacewave/net/stream"
	datagram "github.com/s4wave/spacewave/net/stream/datagram"
)

// Forward forwards an admitted guest's stream to the game server at target
// until ctx ends or either side stops. It binds the guest a socket of its own,
// so the server sees it as a separate client and replies cannot cross guests,
// and calls accept with that socket's address once it is bound, the address
// the server reports the guest's client connecting from. accept, which may be
// nil, lets packets flow, as AcceptGeneration does. Forward owns peer even
// when it fails.
func Forward(ctx context.Context, target *net.UDPAddr, peer stream.MessageStream, accept func(local *net.UDPAddr) error) error {
	// Close the stream when ctx ends, even before forwarding starts.
	ctx, cancel := context.WithCancel(ctx)
	defer cancel()
	defer peer.Close()
	closePeer := context.AfterFunc(ctx, func() { peer.Close() })
	defer closePeer()

	// Bind the guest's socket.
	socket, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		return err
	}
	defer socket.Close()
	if ctx.Err() != nil {
		return ctx.Err()
	}

	// Let packets flow only once the socket is in place.
	if accept != nil {
		if err := accept(socket.LocalAddr().(*net.UDPAddr)); err != nil {
			return err
		}
	}
	return datagram.Forward(ctx, socket, target, peer)
}
