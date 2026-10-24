package peernet

import (
	"bytes"
	"context"
	"crypto/rand"
	"encoding/base64"
	"net"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/aperturerobotics/go-websocket"
	"github.com/aperturerobotics/starpc/srpc"
	"github.com/paralin/modlock/publish"
	"github.com/paralin/modlock/session"
	"github.com/paralin/modlock/src/host_app/control"
	"github.com/paralin/modlock/src/wasm/ui"
	api "github.com/s4wave/spacewave/core/provider/spacewave/api"
	"github.com/s4wave/spacewave/net/peer"
	signaling_rpc "github.com/s4wave/spacewave/net/signaling/rpc"
	signaling_rpc_frame "github.com/s4wave/spacewave/net/signaling/rpc/frame"
	signaling_server "github.com/s4wave/spacewave/net/signaling/rpc/server"
	"github.com/sirupsen/logrus"
)

// signalPeerKey names a socket's peer on its RPC context.
type signalPeerKey struct{}

// fakeSignaling is a signaling service that admits a peer while it holds an
// admission socket with the bearer token good. It trusts the peer header
// instead of checking signatures.
type fakeSignaling struct {
	// good is the token the service accepts.
	good string
	// mux serves the signaling RPCs.
	mux srpc.Mux

	// mtx guards the fields below.
	mtx sync.Mutex
	// admitted counts each peer's open admission sockets.
	admitted map[peer.ID]int
	// tickets maps each unused ticket to its peer.
	tickets map[string]peer.ID
	// lobby is the open dev lobby socket, if any.
	lobby *websocket.Conn

	// lobbyOpens receives each dev lobby socket's request.
	lobbyOpens chan *publish.DevLobbyOpen
	// lobbyCloses receives how each dev lobby socket closed.
	lobbyCloses chan websocket.StatusCode
}

// newFakeSignaling serves signaling and dev lobbies that accept the token
// good.
func newFakeSignaling(t *testing.T, good string) (*httptest.Server, *fakeSignaling) {
	// Name each socket's caller by its ticket's peer.
	f := &fakeSignaling{
		good:        good,
		mux:         srpc.NewMux(),
		admitted:    make(map[peer.ID]int),
		tickets:     make(map[string]peer.ID),
		lobbyOpens:  make(chan *publish.DevLobbyOpen, 4),
		lobbyCloses: make(chan websocket.StatusCode, 4),
	}
	server := signaling_server.NewServerWithIdentify(logrus.NewEntry(logrus.New()), func(ctx context.Context) (peer.ID, error) {
		return ctx.Value(signalPeerKey{}).(peer.ID), nil
	})
	if err := signaling_rpc.SRPCRegisterSignaling(f.mux, server); err != nil {
		t.Fatal(err)
	}

	// Route the admission, ticket and socket paths.
	routes := http.NewServeMux()
	routes.HandleFunc("GET "+AdmitPath, f.serveAdmit)
	routes.HandleFunc("POST /api/signal/ticket", f.serveTicket)
	routes.HandleFunc("GET /api/signal/ws", f.serveSocket)
	routes.HandleFunc("GET "+DevLobbyPath, f.serveDevLobby)
	service := httptest.NewServer(routes)
	t.Cleanup(service.Close)
	return service, f
}

// serveDevLobby opens a lobby for the good token and holds it while the
// socket stays open, reporting the request and how the socket closed.
func (f *fakeSignaling) serveDevLobby(w http.ResponseWriter, r *http.Request) {
	// Refuse any other sign-in.
	if r.Header.Get("Authorization") != "Bearer "+f.good {
		http.Error(w, "Unauthorized", http.StatusUnauthorized)
		return
	}
	conn, err := websocket.Accept(w, r, nil)
	if err != nil {
		return
	}
	defer conn.CloseNow()

	// Answer the request with the lobby's page, keeping the socket for
	// dropLobby first.
	_, data, err := conn.Read(r.Context())
	open := &publish.DevLobbyOpen{}
	if err != nil || open.UnmarshalVT(data) != nil {
		return
	}
	f.lobbyOpens <- open
	f.mtx.Lock()
	f.lobby = conn
	f.mtx.Unlock()
	if f.refreshLobby(r.Context(), "first") != nil {
		return
	}

	// Hold it until it closes.
	_, _, err = conn.Read(r.Context())
	f.lobbyCloses <- websocket.CloseStatus(err)
}

// refreshLobby sends the open dev lobby's page with a relay whose credential
// is named username.
func (f *fakeSignaling) refreshLobby(ctx context.Context, username string) error {
	// Build the state, then send it on the open socket.
	relay := &publish.Relay{
		Servers:   []*publish.RelayServer{{Urls: []string{"stun:127.0.0.1:9", "turn:127.0.0.1:9"}, Username: username, Credential: "secret"}},
		ExpiresAt: time.Now().Add(time.Hour).UnixMilli(),
	}
	state, _ := (&publish.DevLobbyState{State: &publish.DevLobbyState_Url{Url: testLobbyURL}, Relay: relay}).MarshalVT()
	f.mtx.Lock()
	conn := f.lobby
	f.mtx.Unlock()
	return conn.Write(ctx, websocket.MessageBinary, state)
}

// requireRelay waits for the TURN credential service's new links use to be
// named username, or for none when username is empty.
func requireRelay(t *testing.T, service *Service, username string) {
	t.Helper()
	for deadline := time.Now().Add(5 * time.Second); ; time.Sleep(10 * time.Millisecond) {
		config, _ := service.relay.ICEConfig(t.Context())
		var got string
		if servers := config.GetIceServers(); len(servers) > 1 {
			got = servers[1].GetUsername()
		}
		if got == username {
			return
		}
		if time.Now().After(deadline) {
			t.Fatalf("relay credential %q, want %q", got, username)
		}
	}
}

// dropLobby drops the open dev lobby socket without a close.
func (f *fakeSignaling) dropLobby() {
	f.mtx.Lock()
	defer f.mtx.Unlock()
	f.lobby.CloseNow()
}

// testLobbyURL is the page of the fake service's dev lobby.
const testLobbyURL = "https://hyperline.test/play?lobby=dev"

// serveAdmit admits the peer while its socket with the good token is open.
func (f *fakeSignaling) serveAdmit(w http.ResponseWriter, r *http.Request) {
	// Refuse any other sign-in.
	pid, err := peer.IDB58Decode(r.Header.Get("X-Peer-ID"))
	if err != nil || r.Header.Get("Authorization") != "Bearer "+f.good {
		http.Error(w, "Unauthorized", http.StatusUnauthorized)
		return
	}
	conn, err := websocket.Accept(w, r, nil)
	if err != nil {
		return
	}
	defer conn.CloseNow()

	// Count the peer while the socket stays open.
	f.mtx.Lock()
	f.admitted[pid]++
	f.mtx.Unlock()
	defer func() {
		f.mtx.Lock()
		f.admitted[pid]--
		f.mtx.Unlock()
	}()
	<-conn.CloseRead(r.Context()).Done()
}

// serveTicket issues a ticket to an admitted peer, refusing others with 403
// so they retry.
func (f *fakeSignaling) serveTicket(w http.ResponseWriter, r *http.Request) {
	// Refuse a peer without an open admission.
	pid, err := peer.IDB58Decode(r.Header.Get("X-Peer-ID"))
	f.mtx.Lock()
	defer f.mtx.Unlock()
	if err != nil || f.admitted[pid] == 0 {
		http.Error(w, "Forbidden", http.StatusForbidden)
		return
	}

	// Issue a random ticket for it.
	var token [16]byte
	_, _ = rand.Read(token[:])
	encoded := base64.RawURLEncoding.EncodeToString(token[:])
	f.tickets[encoded] = pid
	data, _ := (&api.SignalTicketResponse{Token: encoded}).MarshalVT()
	_, _ = w.Write(data)
}

// serveSocket redeems a ticket and serves its peer's signaling RPCs.
func (f *fakeSignaling) serveSocket(w http.ResponseWriter, r *http.Request) {
	// Redeem the ticket once.
	f.mtx.Lock()
	pid, ok := f.tickets[r.URL.Query().Get("tk")]
	delete(f.tickets, r.URL.Query().Get("tk"))
	f.mtx.Unlock()
	if !ok {
		http.Error(w, "Forbidden", http.StatusForbidden)
		return
	}

	// Serve the peer's RPCs until either side closes.
	conn, err := websocket.Accept(w, r, nil)
	if err != nil {
		return
	}
	defer conn.CloseNow()
	ctx, cancel := context.WithCancel(context.WithValue(r.Context(), signalPeerKey{}, pid))
	defer cancel()
	_ = signaling_rpc_frame.NewConn(ctx, conn).ReadPump(signaling_rpc_frame.NewServerAccept(ctx, f.mux))
}

// awaitAdmission asks s for its peer until its admission reaches want.
func awaitAdmission(ctx context.Context, t *testing.T, s *Service, want AdmissionState) *GetPeerResponse {
	t.Helper()
	for {
		got, err := s.GetPeer(ctx, &GetPeerRequest{})
		if err != nil {
			t.Fatalf("waiting for %v: %v", want, err)
		}
		if got.GetAdmission() == want {
			return got
		}
		select {
		case <-time.After(20 * time.Millisecond):
		case <-ctx.Done():
			t.Fatalf("admission stayed %v (%s), want %v", got.GetAdmission(), got.GetError(), want)
		}
	}
}

// TestServiceProbe walks two PCs' peers through signing in, a refused
// sign-in that a new one replaces, a probe that needs signaling to link them,
// a guest joining the host's game, and the guest signing out.
func TestServiceProbe(t *testing.T) {
	// Serve signaling.
	ctx, cancel := context.WithTimeout(t.Context(), time.Minute)
	defer cancel()
	le := logrus.NewEntry(logrus.New())
	service, signaling := newFakeSignaling(t, "good")
	origin := service.URL

	// Give each PC a key file and a sign-in; the guest's has expired.
	keyPath := filepath.Join(t.TempDir(), "peer.pem")
	host := NewService(origin, keyPath, func() SignIn { return SignIn{Token: "good", Account: "host"} })
	var guestToken atomic.Value
	guestToken.Store("expired")
	guest := NewService(origin, filepath.Join(t.TempDir(), "peer.pem"), func() SignIn {
		return SignIn{Token: guestToken.Load().(string), Account: "guest"}
	})

	// Run both peers.
	for _, s := range []*Service{host, guest} {
		go func() {
			if err := s.Run(ctx, le); err != nil && ctx.Err() == nil {
				t.Error(err)
			}
		}()
	}

	// The host is admitted; the guest's sign-in is refused until it signs in
	// again.
	hostPeer := awaitAdmission(ctx, t, host, AdmissionState_ADMISSION_STATE_ADMITTED)
	if hostPeer.GetAccount() != "host" || hostPeer.GetOrigin() != origin {
		t.Fatalf("host peer is %v", hostPeer)
	}
	awaitAdmission(ctx, t, guest, AdmissionState_ADMISSION_STATE_REFUSED)
	guestToken.Store("good")
	awaitAdmission(ctx, t, guest, AdmissionState_ADMISSION_STATE_ADMITTED)

	// The guest probes the host over a direct link.
	probe, err := guest.Probe(ctx, &ProbeRequest{PeerId: hostPeer.GetPeerId()})
	if err != nil {
		t.Fatal(err)
	}
	if probe.GetRttUs() == 0 {
		t.Fatalf("probe is %v", probe)
	}

	// The guest joins the host's session and its game reaches the server.
	joinSession(ctx, t, host, guest, signaling)

	// Signing the guest out drops its admitted socket.
	guestToken.Store("")
	awaitAdmission(ctx, t, guest, AdmissionState_ADMISSION_STATE_SIGNED_OUT)

	// The host's key stays the same for its next run.
	key, err := LoadKey(keyPath)
	if err != nil {
		t.Fatal(err)
	}
	if id, _ := peer.IDFromPrivateKey(key); id.String() != hostPeer.GetPeerId() {
		t.Fatalf("reloaded key is %s, want %s", id, hostPeer.GetPeerId())
	}
}

// joinSession hosts a game server on host, refuses a guest with the wrong
// session, and walks guest's game through joining it.
func joinSession(ctx context.Context, t *testing.T, host, guest *Service, signaling *fakeSignaling) {
	// Run a game server.
	server, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatal(err)
	}
	defer server.Close()
	server.SetDeadline(time.Now().Add(10 * time.Second))

	// The mod's build carries a model larger than one content part.
	built := t.TempDir()
	model := bytes.Repeat([]byte("pawn"), contentPart/2+1)
	if err := os.MkdirAll(filepath.Join(built, "models", "chess-0.1.0"), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(built, "models", "chess-0.1.0", "pawn.vmdl_c"), model, 0o644); err != nil {
		t.Fatal(err)
	}

	// Host it on its port.
	hostCtx, endHost := context.WithCancel(ctx)
	defer endHost()
	hostClient := NewSRPCPeerNetClient(rpcClient(t, host))
	hosted, err := hostClient.Host(hostCtx)
	if err != nil {
		t.Fatal(err)
	}
	lobby := &LobbyRequest{ModName: "Chess", Version: "0.1.0"}
	start := &HostRequest{Port: uint32(server.LocalAddr().(*net.UDPAddr).Port), Lobby: lobby, Mods: []string{built}}
	if err := hosted.Send(&HostInput{Body: &HostInput_Start{Start: start}}); err != nil {
		t.Fatal(err)
	}

	// The guest's player is in slot 1, where a mod shows a label.
	joinedSlot := &control.PlayerJoined{Slot: 1, SteamId: guestSteamID}
	label := &ui.Change{Reset_: true, Set: []*ui.Node{{Id: "title", Text: "your turn"}}}
	for _, event := range []*control.HostEvent{
		{Body: &control.HostEvent_Joined{Joined: joinedSlot}},
		{Body: &control.HostEvent_Ui{Ui: &control.UiChanged{Mod: "chess", Slot: 1, Change: label}}},
	} {
		if err := hosted.Send(&HostInput{Body: &HostInput_Server{Server: event}}); err != nil {
			t.Fatal(err)
		}
	}

	// Read the join code; a guest with another session is refused.
	first, err := hosted.Recv()
	if err != nil {
		t.Fatal(err)
	}
	code, err := ParseJoinCode(first.GetJoinCode())
	if err != nil {
		t.Fatal(err)
	}

	// The session's dev lobby opens with its secret and relay, a refreshed
	// relay replaces it without repeating the lobby's row, and a dropped
	// socket reconnects to the same lobby.
	open := <-signaling.lobbyOpens
	if open.GetModName() != "Chess" || open.GetVersion() != "0.1.0" || open.GetSecret() != code.Session {
		t.Fatalf("lobby request %v", open)
	}

	// requireLobby reads the next lobby state: the page url, or reconnecting
	// when url is empty.
	requireLobby := func(url string) {
		t.Helper()
		event, err := hosted.Recv()
		state := event.GetLobby()
		if err != nil || state.GetUrl() != url || (url == "" && state.GetReconnecting() == "") {
			t.Fatalf("lobby event %v: %v, want %q", event, err, url)
		}
	}
	requireLobby(testLobbyURL)
	requireRelay(t, host, "first")
	if err := signaling.refreshLobby(ctx, "second"); err != nil {
		t.Fatal(err)
	}
	requireRelay(t, host, "second")

	// The refresh printed nothing, so the next state is the reconnect.
	signaling.dropLobby()
	if status := <-signaling.lobbyCloses; status != -1 {
		t.Fatalf("dropped lobby closed with %v", status)
	}
	requireLobby("")
	<-signaling.lobbyOpens
	requireLobby(testLobbyURL)
	guestClient := NewSRPCPeerNetClient(rpcClient(t, guest))
	wrong := JoinCode{Host: code.Host, Session: code.Session + 1}
	if refused, err := guestClient.Join(ctx, &JoinRequest{Code: wrong.String()}); err == nil {
		if _, err := refused.Recv(); err == nil {
			t.Fatal("the host admitted the wrong session")
		}
	}

	// The guest joins, receiving the mod's content whole before the address,
	// and the host reports it.
	joined, err := guestClient.Join(ctx, &JoinRequest{Code: code.String(), SteamId: guestSteamID})
	if err != nil {
		t.Fatal(err)
	}
	var endpoint *JoinEvent
	var received []byte
	for endpoint == nil {
		event, err := joined.Recv()
		if err != nil {
			t.Fatal(err)
		}
		if part := event.GetContent(); part != nil {
			if part.GetPath() != "models/chess-0.1.0/pawn.vmdl_c" {
				t.Fatalf("content part %q", part.GetPath())
			}
			received = append(received, part.GetData()...)
		} else if event.GetAddress() != "" {
			endpoint = event
		}
	}
	if !bytes.Equal(received, model) {
		t.Fatalf("received %d bytes of content, want %d", len(received), len(model))
	}
	if event, err := hosted.Recv(); err != nil || event.GetJoined() != guest.local.String() {
		t.Fatalf("host event %v: %v", event, err)
	}

	// The guest's game reaches the server and the guest sees it arrive.
	game, err := net.Dial("udp4", endpoint.GetAddress())
	if err != nil {
		t.Fatal(err)
	}
	defer game.Close()
	if _, err := game.Write([]byte("hello")); err != nil {
		t.Fatal(err)
	}
	packet := make([]byte, 64)
	if n, _, err := server.ReadFromUDP(packet); err != nil || string(packet[:n]) != "hello" {
		t.Fatalf("server read %q: %v", packet[:n], err)
	}

	// The guest sees its game arrive and its own screen.
	mirror := session.NewMirror()
	for arrived := false; !arrived || mirrorText(mirror) != "your turn"; {
		event, err := joined.Recv()
		if err != nil {
			t.Fatalf("join event: %v", err)
		}
		arrived = arrived || event.GetArrived()
		if frame := event.GetScreen(); frame != nil {
			mirror.Apply(frame.GetChanges(), frame.GetReset_())
		}
	}

	// A line said in the game reaches the guest.
	said := &control.PlayerChat{Name: "Host", Text: "gg"}
	if err := hosted.Send(&HostInput{Body: &HostInput_Server{Server: &control.HostEvent{Body: &control.HostEvent_Chat{Chat: said}}}}); err != nil {
		t.Fatal(err)
	}
	for line := (*control.PlayerChat)(nil); line == nil; {
		event, err := joined.Recv()
		if err != nil {
			t.Fatalf("join event: %v", err)
		}
		if line = event.GetChat(); line != nil && line.GetName()+": "+line.GetText() != "Host: gg" {
			t.Fatalf("chat line %v", line)
		}
	}

	// Ending the session closes its lobby normally and drops its relay.
	endHost()
	if status := <-signaling.lobbyCloses; status != websocket.StatusNormalClosure {
		t.Fatalf("ended lobby closed with %v", status)
	}
	requireRelay(t, host, "")
}

// guestSteamID is the guest player's Steam ID.
const guestSteamID = 76561198000000007

// mirrorText returns the text of the chess mod's title on mirror's screen.
func mirrorText(mirror *session.Mirror) string {
	screen, _ := mirror.Screen()
	for _, tree := range screen.GetTrees() {
		for _, node := range tree.GetNodes() {
			if tree.GetMod() == "chess" && node.GetId() == "title" {
				return node.GetText()
			}
		}
	}
	return ""
}

// rpcClient calls s's PeerNet service in memory.
func rpcClient(t *testing.T, s *Service) srpc.Client {
	mux := srpc.NewMux()
	if err := s.Register(mux); err != nil {
		t.Fatal(err)
	}
	return srpc.NewClient(srpc.NewServerPipe(srpc.NewServer(mux)))
}
