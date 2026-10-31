package peernet

import (
	"context"
	"crypto/rand"
	"encoding/binary"
	"io"
	"maps"
	"math"
	"net"
	"slices"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/aperturerobotics/controllerbus/bus"
	"github.com/aperturerobotics/controllerbus/controller"
	"github.com/aperturerobotics/controllerbus/directive"
	"github.com/aperturerobotics/starpc/srpc"
	"github.com/paralin/modlock/gamelink"
	"github.com/paralin/modlock/session"
	"github.com/paralin/modlock/src/wasm/ui"
	"github.com/pkg/errors"
	"github.com/s4wave/spacewave/net/link"
	"github.com/s4wave/spacewave/net/peer"
	"github.com/s4wave/spacewave/net/protocol"
	"github.com/s4wave/spacewave/net/stream"
	stream_srpc "github.com/s4wave/spacewave/net/stream/srpc"
)

// GameProtocolID identifies game streams between modlock peers, which
// gamelink forwards. The handshake's generation is the hosted session, so
// only a guest holding the join code reaches the server.
const GameProtocolID = protocol.ID("modlock/game/1")

// SessionProtocolID identifies the Session service a host serves its guests'
// daemons, one call per stream.
const SessionProtocolID = protocol.ID("modlock/session/1")

// JoinCode names a hosted session: the host's peer and the session's secret.
type JoinCode struct {
	// Host is the hosting PC's peer.
	Host peer.ID
	// Session is the session's nonzero secret.
	Session uint64
}

// String formats the code as the peer, a dash and the session in hex. Peer
// IDs are base58, which has no dash.
func (c JoinCode) String() string {
	return c.Host.String() + "-" + strconv.FormatUint(c.Session, 16)
}

// ParseJoinCode reads a code that JoinCode.String formatted.
func ParseJoinCode(code string) (JoinCode, error) {
	// Split the peer from the session.
	host, session, ok := strings.Cut(strings.TrimSpace(code), "-")
	if !ok {
		return JoinCode{}, errors.New("a join code is the host's peer, a dash and the session")
	}
	id, err := peer.IDB58Decode(host)
	if err != nil {
		return JoinCode{}, errors.Wrap(err, "read the join code's peer")
	}

	// Read the session, which is never zero.
	secret, err := strconv.ParseUint(session, 16, 64)
	if err != nil || secret == 0 {
		return JoinCode{}, errors.New("the join code's session is not valid")
	}
	return JoinCode{Host: id, Session: secret}, nil
}

// hosting answers game streams to the local peer that name its session,
// forwarding each guest to the game server, while its controller runs.
type hosting struct {
	// ctx ends the hosted session and every guest's forwarding.
	ctx context.Context
	// bus holds each guest's link while the host serves it.
	bus bus.Bus
	// local is the hosting peer.
	local peer.ID
	// session is the join code's secret.
	session uint64
	// target is the game server.
	target *net.UDPAddr
	// events receives each guest that joins or leaves.
	events func(*HostEvent)
	// sessions serves the Session service to guests.
	sessions *srpc.Server
}

// GetControllerInfo identifies the hosted session.
func (h *hosting) GetControllerInfo() *controller.Info {
	return controller.NewInfo("modlock/peernet/hosting", controller.MustParseVersion("0.0.1"), "hosted game session")
}

// Execute does nothing; guests arrive as streams.
func (h *hosting) Execute(context.Context) error { return nil }

// Close does nothing; forwarding ends with ctx.
func (h *hosting) Close() error { return nil }

// HandleDirective accepts game and session streams to the local peer.
func (h *hosting) HandleDirective(_ context.Context, inst directive.Instance) ([]directive.Resolver, error) {
	d, ok := inst.GetDirective().(link.HandleMountedStream)
	if !ok || d.HandleMountedStreamLocalPeerID() != h.local {
		return nil, nil
	}
	switch d.HandleMountedStreamProtocolID() {
	case GameProtocolID, SessionProtocolID:
		return directive.Resolvers(directive.NewValueResolver([]link.MountedStreamHandler{h})), nil
	default:
		return nil, nil
	}
}

// HandleMountedStream serves a session stream's call, and forwards a guest
// whose game stream names the session to the server until the guest or the
// session ends, closing any other stream. Each stream holds the guest's link
// while it is served.
func (h *hosting) HandleMountedStream(_ context.Context, mounted link.MountedStream) error {
	// Serve a session call; the call checks the join code.
	if mounted.GetProtocolID() == SessionProtocolID {
		go func() {
			defer h.holdLink(mounted.GetPeerID())()
			h.sessions.HandleStream(h.ctx, mounted.GetStream())
		}()
		return nil
	}

	// Take the guest's game stream.
	messages, ok := mounted.GetStream().(stream.MessageStream)
	if !ok {
		mounted.GetStream().Close()
		return errors.New("the game stream carries no messages")
	}
	guest := mounted.GetPeerID().String()
	go func() {
		defer h.holdLink(mounted.GetPeerID())()

		// Refuse a guest without the join code.
		control := messages.Control()
		generation, err := gamelink.ReadGeneration(h.ctx, control)
		if err != nil || generation != h.session {
			messages.Close()
			return
		}

		// Forward the guest, reporting when it joins and leaves.
		h.events(&HostEvent{Body: &HostEvent_Joined{Joined: guest}})
		_ = gamelink.Forward(h.ctx, h.target, messages, func() error { return gamelink.AcceptGeneration(h.ctx, control) })
		h.events(&HostEvent{Body: &HostEvent_Left{Left: guest}})
	}()
	return nil
}

// holdLink keeps the link to guest open until the returned release is called.
// A link the guest dialed stays open only while a directive references it on
// this side too; without one the transport closes it seconds after it opens,
// in the middle of the guest's game.
func (h *hosting) holdLink(guest peer.ID) func() {
	_, ref, err := h.bus.AddDirective(h.ctx, link.NewEstablishLinkWithPeer(h.local, guest), nil)
	if err != nil {
		return func() {}
	}
	return ref.Release
}

// Host serves the local game server to guests holding the join code it sends
// first, until the call ends. The caller names the server, then sends its
// players and interfaces, which the host streams to each guest as their own
// screen. When the caller asks for a dev lobby, the host holds it open at the
// signaling service and reports where it stands; the call ending closes it.
func (s *Service) Host(strm SRPCPeerNet_HostStream) error {
	// Read the server's port.
	ctx, cancel := context.WithCancel(strm.Context())
	defer cancel()
	start, err := strm.Recv()
	if err != nil {
		return err
	}
	port := start.GetStart().GetPort()
	if port == 0 || port > 65535 {
		return errors.New("name the game server's port")
	}

	// Hold the daemon's one session once its peer is up.
	if err := await(ctx, s.ready); err != nil {
		return err
	}
	if !s.hosting.CompareAndSwap(false, true) {
		return errors.New("this PC already hosts a session")
	}
	defer s.hosting.Store(false)

	// Follow the server's players, interfaces and chat until the caller
	// stops.
	screens, chat := session.NewScreens(0), session.NewChat()
	go func() {
		defer cancel()
		for {
			in, err := strm.Recv()
			if err != nil {
				return
			}
			event := in.GetServer()
			if line := event.GetChat(); line != nil {
				chat.Add(line)
			}
			if event != nil {
				screens.Handle(event)
			}
		}
	}()

	// Pick the session's secret.
	var secret [8]byte
	for binary.BigEndian.Uint64(secret[:]) == 0 {
		_, _ = rand.Read(secret[:])
	}
	code := JoinCode{Host: s.local, Session: binary.BigEndian.Uint64(secret[:])}

	// Answer guests, sending each change in order.
	events := make(chan *HostEvent, 16)
	events <- &HostEvent{Body: &HostEvent_JoinCode{JoinCode: code.String()}}
	guests := &sessionServer{session: code.Session, mods: start.GetStart().GetMods(), screens: screens, chat: chat}
	h, err := s.newHosting(ctx, int(port), guests, func(event *HostEvent) {
		select {
		case events <- event:
		case <-ctx.Done():
		}
	})
	if err != nil {
		return err
	}
	release, err := s.node.GetBus().AddController(ctx, h, nil)
	if err != nil {
		return err
	}
	defer release()

	// Hold the dev lobby beside the session, when asked for.
	if lobby := start.GetStart().GetLobby(); lobby != nil {
		lobbyDone := make(chan struct{})
		defer func() {
			cancel()
			<-lobbyDone
		}()
		go func() {
			defer close(lobbyDone)
			s.holdLobby(ctx, lobby, code.Session, func(state *LobbyState) {
				h.events(&HostEvent{Body: &HostEvent_Lobby{Lobby: state}})
			})
		}()
	}

	// Send events until the call ends.
	for {
		select {
		case <-ctx.Done():
			return nil
		case event := <-events:
			if err := strm.Send(event); err != nil {
				return err
			}
		}
	}
}

// newHosting returns the session that forwards guests to the server on port
// and serves them what they see from guests.
func (s *Service) newHosting(ctx context.Context, port int, guests *sessionServer, events func(*HostEvent)) (*hosting, error) {
	mux := srpc.NewMux()
	if err := SRPCRegisterSession(mux, guests); err != nil {
		return nil, err
	}
	return &hosting{
		ctx:      ctx,
		bus:      s.node.GetBus(),
		local:    s.local,
		session:  guests.session,
		target:   &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: port},
		events:   events,
		sessions: srpc.NewServer(mux),
	}, nil
}

// sessionServer serves a hosted session's Session service.
type sessionServer struct {
	// session is the join code's secret.
	session uint64
	// mods are the directories of the built mods the server runs.
	mods []string
	// screens holds every player's screen.
	screens *session.Screens
	// chat holds the newest lines said.
	chat *session.Chat
}

// Watch streams what the guest sees: the whole screen first, then at most
// one frame of changes and new chat lines each FrameGap. Comparing screens
// instead of queuing the mods' changes means a slow guest gets the newest
// screen, never a backlog.
func (s *sessionServer) Watch(req *WatchRequest, strm SRPCSession_WatchStream) error {
	// Refuse a guest without the join code.
	ctx := strm.Context()
	if req.GetSession() != s.session {
		return errors.New("the join code names another session")
	}

	// Send the screen whole, then each change and each line said from now on.
	sent := &ui.Screen{}
	_, said, _ := s.chat.Since(math.MaxUint64)
	for reset := true; ; reset = false {
		lines, next, spoken := s.chat.Since(said)
		for _, line := range lines {
			if err := strm.Send(&SessionEvent{Body: &SessionEvent_Chat{Chat: line}}); err != nil {
				return err
			}
		}
		said = next

		// Send the screen's changes, if any.
		screen, changed := s.screens.Player(req.GetSteamId())
		if changes := session.Changes(sent, screen); reset || len(changes) != 0 {
			frame := &ScreenFrame{Changes: changes, Reset_: reset}
			if err := strm.Send(&SessionEvent{Body: &SessionEvent_Screen{Screen: frame}}); err != nil {
				return err
			}
			sent = screen
		}
		if !waitFrame(ctx, changed, spoken) {
			return nil
		}
	}
}

// contentPart is the most of a file one ContentPart carries, well under the
// largest message a stream takes.
const contentPart = 1 << 20

// Content streams the hosted mods' content addons, read from their build
// directories on each call so a guest gets the newest build.
func (s *sessionServer) Content(req *ContentRequest, strm SRPCSession_ContentStream) error {
	// Refuse a guest without the join code.
	if req.GetSession() != s.session {
		return errors.New("the join code names another session")
	}
	files, err := session.ReadContent(s.mods)
	if err != nil {
		return err
	}

	// Send each file in parts, in path order.
	for _, path := range slices.Sorted(maps.Keys(files)) {
		data := files[path].Data
		for first := true; first || len(data) != 0; first = false {
			n := min(len(data), contentPart)
			if err := strm.Send(&ContentPart{Path: path, Data: data[:n]}); err != nil {
				return err
			}
			data = data[n:]
		}
	}
	return nil
}

// waitFrame waits for changed or spoken, then for the gap between frames, and
// reports whether ctx is still open.
func waitFrame(ctx context.Context, changed, spoken <-chan struct{}) bool {
	select {
	case <-changed:
	case <-spoken:
	case <-ctx.Done():
		return false
	}
	select {
	case <-time.After(session.FrameGap):
		return true
	case <-ctx.Done():
		return false
	}
}

// Join links to the session the code names and serves it to the local game
// on a loopback port until the game leaves or the call ends.
func (s *Service) Join(req *JoinRequest, strm SRPCPeerNet_JoinStream) error {
	// Read the code and wait for the local peer.
	ctx := strm.Context()
	code, err := ParseJoinCode(req.GetCode())
	if err != nil {
		return err
	}
	if err := await(ctx, s.ready); err != nil {
		return err
	}

	// Link to the host and wait for it to accept the session.
	client, err := gamelink.Dial(ctx, s.node.GetBus(), GameProtocolID, s.local, code.Host, code.Session)
	if err != nil {
		return err
	}
	defer client.Close()
	var sending sync.Mutex
	send := func(event *JoinEvent) error {
		sending.Lock()
		defer sending.Unlock()
		return strm.Send(event)
	}

	// Send the mods' content, then the address, so the game mounts the
	// content before it starts. Without the content the game still plays,
	// drawing the mods' resources as errors.
	host := NewSRPCSessionClient(srpc.NewClient(stream_srpc.NewOpenStreamFunc(s.node.GetBus(), SessionProtocolID, s.local, code.Host, 0)))
	for _, part := range fetchContent(ctx, host, code.Session) {
		if err := send(&JoinEvent{Body: &JoinEvent_Content{Content: part}}); err != nil {
			return err
		}
	}
	if err := send(&JoinEvent{Body: &JoinEvent_Address{Address: client.Address()}}); err != nil {
		return err
	}

	// Stream what the player sees while the game plays.
	if req.GetSteamId() != 0 {
		watch, cancel := context.WithCancel(ctx)
		defer cancel()
		go s.watch(watch, host, &WatchRequest{Session: code.Session, SteamId: req.GetSteamId()}, send)
	}

	// Report the game's arrival, then hold until it leaves.
	select {
	case <-client.Arrived():
		if err := send(&JoinEvent{Body: &JoinEvent_Arrived{Arrived: true}}); err != nil {
			return err
		}
	case <-client.Done():
	}
	if err := client.Wait(); err != nil && !errors.Is(err, gamelink.ErrGameLeft) && ctx.Err() == nil {
		return err
	}
	return nil
}

// contentWait bounds fetching a session's content, so a host that never
// answers delays the game only this long.
const contentWait = 2 * time.Minute

// fetchContent returns every part of the session's content, or nothing when
// the host cannot send all of it, since a partial file would break the
// package.
func fetchContent(ctx context.Context, host SRPCSessionClient, session uint64) []*ContentPart {
	ctx, cancel := context.WithTimeout(ctx, contentWait)
	defer cancel()
	parts, err := host.Content(ctx, &ContentRequest{Session: session})
	if err != nil {
		return nil
	}
	var all []*ContentPart
	for {
		part, err := parts.Recv()
		switch {
		case errors.Is(err, io.EOF):
			return all
		case err != nil:
			return nil
		}
		all = append(all, part)
	}
}

// watch forwards what the player sees from the host until ctx ends, watching
// again after a lost stream. Each watch starts with the whole screen, so the
// guest never applies a change to a screen it lacks.
func (s *Service) watch(ctx context.Context, host SRPCSessionClient, req *WatchRequest, send func(*JoinEvent) error) {
	for {
		// Forward each frame and line until the stream ends.
		events, err := host.Watch(ctx, req)
		for err == nil {
			var event *SessionEvent
			if event, err = events.Recv(); err == nil {
				err = send(joinEvent(event))
			}
		}

		// Watch again after a pause, unless the call ended.
		select {
		case <-ctx.Done():
			return
		case <-time.After(time.Second):
		}
	}
}

// joinEvent returns the Join event that reports a session event.
func joinEvent(event *SessionEvent) *JoinEvent {
	if line := event.GetChat(); line != nil {
		return &JoinEvent{Body: &JoinEvent_Chat{Chat: line}}
	}
	return &JoinEvent{Body: &JoinEvent_Screen{Screen: event.GetScreen()}}
}
