package gamelink

import (
	"context"
	"errors"
	"io"
	"net"
	"testing"
	"time"

	"github.com/s4wave/spacewave/net/stream"
)

// TestGameLink walks two guests through admission to one game server, checks
// that the server sees each as its own client, then cancels an admission the
// host never answers and ends a game that goes quiet.
func TestGameLink(t *testing.T) {
	// Run a game server that answers each packet to its sender.
	ctx, cancel := context.WithTimeout(t.Context(), 10*time.Second)
	defer cancel()
	server, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatal(err)
	}
	defer server.Close()
	server.SetDeadline(time.Now().Add(5 * time.Second))
	target := server.LocalAddr().(*net.UDPAddr)

	// Each guest reaches the server from its own source.
	sources := make(map[string]string)
	for _, payload := range []string{"player one", "player two"} {
		source := joinGame(ctx, t, server, target, payload)
		if other, ok := sources[source]; ok {
			t.Fatalf("%q and %q share the source %s", payload, other, source)
		}
		sources[source] = payload
	}

	// A guest whose host never answers stops when its caller cancels.
	pending, cancelPending := context.WithCancel(ctx)
	guest, host := net.Pipe()
	defer guest.Close()
	defer host.Close()

	// Cancel once the host has the request but has not answered.
	done := make(chan error, 1)
	go func() { done <- requestAdmission(pending, guest, 42) }()
	if _, err := io.ReadFull(host, make([]byte, 8)); err != nil {
		t.Fatal(err)
	}
	cancelPending()
	if err := <-done; !errors.Is(err, context.Canceled) {
		t.Fatalf("pending admission: %v", err)
	}

	// An arrived game that stops sending leaves; one still sending stays.
	local := &arrivalStream{sent: make(chan struct{}), arrived: make(chan struct{})}
	left := make(chan struct{})
	go local.watchQuiet(ctx, 50*time.Millisecond, func() { close(left) })
	time.Sleep(100 * time.Millisecond)

	// Steady packets keep the game.
	local.last.Store(time.Now().UnixNano())
	close(local.sent)
	for range 5 {
		time.Sleep(20 * time.Millisecond)
		local.last.Store(time.Now().UnixNano())
	}
	select {
	case <-left:
		t.Fatal("left while sending")
	default:
	}

	// Silence ends it, as unanswered since no server packet arrived.
	select {
	case <-left:
	case <-time.After(time.Second):
		t.Fatal("quiet game did not leave")
	}
	if err := local.quietCause(); !errors.Is(err, ErrUnanswered) {
		t.Fatalf("quiet cause %v", err)
	}
}

// joinGame admits one guest to the server over a stream pipe, sends payload
// from its game, checks the reply, and returns the source the server saw.
func joinGame(ctx context.Context, t *testing.T, server *net.UDPConn, target *net.UDPAddr, payload string) string {
	// The host accepts the generation it runs and forwards the guest.
	t.Helper()
	upstream, downstream := stream.NewMessagePipe(1200)
	go func() {
		generation, err := ReadGeneration(ctx, upstream.Control())
		if err != nil || generation != 42 {
			t.Errorf("generation %d: %v", generation, err)
			upstream.Close()
			return
		}
		_ = Forward(ctx, target, upstream, func() error { return AcceptGeneration(ctx, upstream.Control()) })
	}()

	// The guest asks for that generation and opens its endpoint.
	if err := requestAdmission(ctx, downstream.Control(), 42); err != nil {
		t.Fatal(err)
	}
	client, err := Open(ctx, downstream)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(client.Close)

	// The game sends through the endpoint, which reports its arrival.
	game, err := net.Dial("udp4", client.Address())
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { game.Close() })
	game.SetDeadline(time.Now().Add(5 * time.Second))

	// The server receives the packet.
	if _, err := game.Write([]byte(payload)); err != nil {
		t.Fatal(err)
	}
	packet := make([]byte, 128)
	n, source, err := server.ReadFromUDP(packet)
	if err != nil || string(packet[:n]) != payload {
		t.Fatalf("server read %q: %v", packet[:n], err)
	}
	select {
	case <-client.Arrived():
		t.Fatal("arrived before the server answered")
	default:
	}

	// The server's reply reaches only this game.
	if _, err := server.WriteToUDP([]byte("ack "+payload), source); err != nil {
		t.Fatal(err)
	}
	n, err = game.Read(packet)
	if err != nil || string(packet[:n]) != "ack "+payload {
		t.Fatalf("game read %q: %v", packet[:n], err)
	}
	<-client.Arrived()
	return source.String()
}
