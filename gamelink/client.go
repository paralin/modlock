// Package gamelink carries a game's UDP traffic between two PCs over a peer
// stream. The guest's game connects to a loopback endpoint; the host forwards
// each admitted guest to its game server with a socket of its own, so the
// server sees every guest as a separate client. Each stream opens with a
// handshake that names the server process the guest expects, and game packets
// flow only once the host accepts it.
package gamelink

import (
	"context"
	"errors"
	"net"
	"sync/atomic"
	"time"

	"github.com/s4wave/spacewave/net/stream"
	datagram "github.com/s4wave/spacewave/net/stream/datagram"
)

// quietLimit is how long an arrived game may stop sending before the endpoint
// treats it as gone. A connected Deadlock client sends every tick; the limit
// leaves room for a slow map load, which can pause it.
const quietLimit = 2 * time.Minute

// ErrGameLeft reports that the game stopped sending after the server answered
// it: the player left the server or quit Deadlock.
var ErrGameLeft = errors.New("Left the game")

// ErrUnanswered reports that the game stopped sending before the server ever
// answered it: the game gave up connecting to a server that never replied.
var ErrUnanswered = errors.New("The host's game did not answer. Choose Join again.")

// Client owns one loopback endpoint until the peer disconnects, the game
// leaves, or Close returns. It serves one server process and is not reused.
type Client struct {
	// address is the loopback endpoint the game connects to.
	address string
	// arrived closes on the server's first packet to the game.
	arrived chan struct{}
	// cancel stops forwarding with its cause.
	cancel context.CancelCauseFunc
	// done closes once forwarding has stopped.
	done chan struct{}
	// err is the forwarding result, set before done closes.
	err error
}

// Open takes ownership of an admitted game stream and binds the loopback
// endpoint for it. The caller then points the game at Address; no process is
// launched.
func Open(ctx context.Context, peer stream.MessageStream) (*Client, error) {
	return open(ctx, peer, nil)
}

// open binds the loopback endpoint and forwards it to peer until ctx ends or
// the game leaves, then calls release.
func open(ctx context.Context, peer stream.MessageStream, release func()) (*Client, error) {
	// Bind the endpoint.
	socket, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		peer.Close()
		return nil, err
	}
	ctx, cancel := context.WithCancelCause(ctx)
	client := &Client{address: socket.LocalAddr().String(), arrived: make(chan struct{}), cancel: cancel, done: make(chan struct{})}
	local := &arrivalStream{MessageStream: peer, sent: make(chan struct{}), arrived: client.arrived}

	// Forward until either side stops or the game goes quiet.
	go func() {
		// Release the link and report the result once forwarding stops.
		defer close(client.done)
		if release != nil {
			defer release()
		}
		defer cancel(nil)

		// Forward, ending when the game goes quiet: ErrGameLeft once the
		// server answered it, ErrUnanswered when it never did.
		go local.watchQuiet(ctx, quietLimit, func() { cancel(local.quietCause()) })
		client.err = datagram.ForwardLocal(ctx, socket, local)
		if cause := context.Cause(ctx); errors.Is(cause, ErrGameLeft) || errors.Is(cause, ErrUnanswered) {
			client.err = cause
		}
	}()
	return client, nil
}

// Address is the game's connect endpoint, valid until Done closes.
func (c *Client) Address() string { return c.address }

// Arrived closes when the server's first packet reaches the game, so the
// game's own connect attempts alone never count as arrival.
func (c *Client) Arrived() <-chan struct{} { return c.arrived }

// Done closes after forwarding has stopped.
func (c *Client) Done() <-chan struct{} { return c.done }

// Wait returns the forwarding result after both directions stop.
// ErrGameLeft means the game went quiet after it arrived, and ErrUnanswered
// that it went quiet before the server ever answered.
func (c *Client) Wait() error { <-c.done; return c.err }

// Close cancels forwarding and waits for sockets and pumps to close.
func (c *Client) Close() { c.cancel(nil); <-c.done }

// arrivalStream records when the game last sent a packet and closes sent on
// its first one, then closes arrived on the server's first packet back. Only
// the forwarder's single socket pump calls Write, and its single peer pump
// calls Read.
type arrivalStream struct {
	stream.MessageStream
	// sent closes on the game's first packet.
	sent chan struct{}
	// arrived closes on the server's first packet.
	arrived chan struct{}
	// last is the UnixNano time of the latest local packet.
	last atomic.Int64
}

// Write marks the game's activity, then forwards the packet.
func (s *arrivalStream) Write(packet []byte) (int, error) {
	s.last.Store(time.Now().UnixNano())
	closeOnce(s.sent)
	return s.MessageStream.Write(packet)
}

// Read takes the server's next packet and marks arrival on the first.
func (s *arrivalStream) Read(packet []byte) (int, error) {
	n, err := s.MessageStream.Read(packet)
	if n > 0 {
		closeOnce(s.arrived)
	}
	return n, err
}

// quietCause names why a quiet game's endpoint ends.
func (s *arrivalStream) quietCause() error {
	select {
	case <-s.arrived:
		return ErrGameLeft
	default:
		return ErrUnanswered
	}
}

// watchQuiet calls left once the game, after its first packet, has sent
// nothing for limit. The timer runs to each packet's known expiry instead of
// sampling.
func (s *arrivalStream) watchQuiet(ctx context.Context, limit time.Duration, left func()) {
	// Wait for the game's first packet.
	select {
	case <-ctx.Done():
		return
	case <-s.sent:
	}

	// Leave once the game has been quiet for limit.
	timer := time.NewTimer(limit)
	defer timer.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-timer.C:
		}
		quiet := time.Since(time.Unix(0, s.last.Load()))
		if quiet >= limit {
			left()
			return
		}
		timer.Reset(limit - quiet)
	}
}

// closeOnce closes a channel that only its one writer closes.
func closeOnce(ch chan struct{}) {
	select {
	case <-ch:
	default:
		close(ch)
	}
}
