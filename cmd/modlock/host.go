package main

import (
	"context"
	"io"
	"net/url"
	"slices"
	"strconv"
	"sync"
	"testing/fstest"

	"github.com/aperturerobotics/cli"
	modcli "github.com/paralin/modlock/cli"
	"github.com/paralin/modlock/peernet"
	modpublish "github.com/paralin/modlock/publish"
	"github.com/paralin/modlock/sandbox"
	"github.com/paralin/modlock/session"
	"github.com/paralin/modlock/src/host_app/control"
	"github.com/pkg/errors"
	"github.com/s4wave/spacewave/net/peer"
)

// hostCommand runs a mod like modlock dev --watch and serves its server to
// guests on other PCs through this PC's peer.
func hostCommand(out *printer) *cli.Command {
	return &cli.Command{
		Name:      "host",
		Usage:     "run the mod like dev --watch and let players on other PCs join with modlock join",
		ArgsUsage: "[DIRECTORY]",
		Flags: append(
			slices.Clone(sessionFlags),
			&cli.UintFlag{
				Name:  "preview-port",
				Value: previewPort,
				Usage: "the loopback port of the page that draws the mod's interface; 0 turns it off",
			},
			&cli.BoolFlag{
				Name:  "no-lobby",
				Usage: "host by join code alone, without a lobby on hyperline.gg",
			},
		),
		Action: func(c *cli.Context) error {
			out.brief = true
			return runDev(c, out, "host", true)
		},
	}
}

// hostSession serves running to guests until the command ends, and sends the
// daemon each player's interface and chat from feed, and the content addons
// of the mods built at mods, which guests mount. With lobby set it puts
// the session on hyperline.gg as a dev lobby and prints its link, falling back
// to the join code when no lobby opens; with lobby nil it prints the join code.
// The server itself reports each player who joins or leaves. Hosting is
// optional: when it cannot start, the session keeps running for this PC alone.
func hostSession(c *cli.Context, out *printer, running server, feed *hostFeed, lobby *peernet.LobbyRequest, mods []string) {
	// Only a game server can take guests.
	if _, sandboxed := running.(*sandbox.Sandbox); sandboxed {
		out.mark("!", yellow, "guests join only a game server, not the sandbox")
		return
	}

	// Ask the daemon to host the server's port.
	client, err := connectDaemon(c.Context)
	if err != nil {
		out.mark("!", yellow, "hosting is off: "+err.Error())
		return
	}
	defer client.Close()
	stream, err := peernet.NewSRPCPeerNetClient(client.RPC()).Host(c.Context)
	if err == nil {
		start := &peernet.HostRequest{Port: uint32(c.Uint("port")), Lobby: lobby, Mods: mods}
		err = stream.Send(&peernet.HostInput{Body: &peernet.HostInput_Start{Start: start}})
	}
	if err != nil {
		out.mark("!", yellow, "hosting is off: "+err.Error())
		return
	}
	go feed.send(c.Context, stream)

	// Print each change until the session ends.
	shown := &hostOutput{out: out, lobby: lobby != nil}
	for {
		event, err := stream.Recv()
		if err != nil {
			if c.Context.Err() == nil {
				out.mark("!", yellow, "hosting stopped: "+err.Error())
			}
			return
		}
		shown.show(event)
	}
}

// hostOutput prints a hosted session's join code and lobby as they change.
type hostOutput struct {
	// out prints the lines.
	out *printer
	// lobby is true while the session may still get a dev lobby, so the join
	// code waits for its answer.
	lobby bool
	// code is the session's join code, once the daemon sent it.
	code string
	// codeShown is true once the join code was printed.
	codeShown bool
	// url is the open lobby's page, once one opened.
	url string
	// reconnecting is true while the lobby's socket is down.
	reconnecting bool
	// hosting is the state --json reports.
	hosting modcli.Hosting
}

// show prints what event changed, or with --json reports where hosting stands.
func (o *hostOutput) show(event *peernet.HostEvent) {
	if o.out.json {
		o.report(event)
		return
	}

	// Print the join code unless a lobby may replace it.
	if code := event.GetJoinCode(); code != "" {
		o.code = code
		if !o.lobby {
			o.showCode()
		}
		return
	}
	lobby := event.GetLobby()
	if lobby == nil {
		return
	}

	// Print the lobby's link, or why there is none.
	switch state := lobby.GetState().(type) {
	case *peernet.LobbyState_Url:
		switch {
		case state.Url == o.url:
			o.out.mark("●", green, "lobby: back")
		case o.url != "":
			o.out.mark("!", yellow, "lobby closed: the connection to hyperline.gg was lost")
			fallthrough
		default:
			o.out.mark("●", green, "hosting at "+state.Url+"; invite friends there")
		}
		o.url, o.reconnecting = state.Url, false
	case *peernet.LobbyState_Reconnecting:
		if !o.reconnecting {
			o.out.mark("…", yellow, "lobby: reconnecting")
		}
		o.reconnecting = true
	case *peernet.LobbyState_Refused:
		o.lobby = false
		o.showCode()
		o.out.mark("!", yellow, state.Refused)
	case *peernet.LobbyState_SignedOut:
		o.lobby = false
		o.showCode()
		o.out.note("modlock login puts it on hyperline.gg")
	}
}

// report emits a Hosting event with the join code and the lobby's state after
// event. A lobby state the event does not change stays as it was.
func (o *hostOutput) report(event *peernet.HostEvent) {
	// Take the code or the lobby's new state.
	if code := event.GetJoinCode(); code != "" {
		o.hosting.JoinCode = code
	}
	switch state := event.GetLobby().GetState().(type) {
	case *peernet.LobbyState_Url:
		o.hosting.Lobby = &modcli.Hosting_LobbyUrl{LobbyUrl: state.Url}
	case *peernet.LobbyState_Reconnecting:
		o.hosting.Lobby = &modcli.Hosting_Reconnecting{Reconnecting: true}
	case *peernet.LobbyState_Refused:
		o.hosting.Lobby = &modcli.Hosting_Unlisted{Unlisted: state.Refused}
	case *peernet.LobbyState_SignedOut:
		o.hosting.Lobby = &modcli.Hosting_Unlisted{Unlisted: "modlock login puts it on hyperline.gg"}
	}

	// Report once the code is known, since players join by it or the lobby.
	if o.hosting.JoinCode != "" {
		o.out.emit(&modcli.Event{Body: &modcli.Event_Hosting{Hosting: o.hosting.CloneVT()}})
	}
}

// showCode prints the join code once.
func (o *hostOutput) showCode() {
	if o.code != "" && !o.codeShown {
		o.out.mark("●", green, "hosting; on the other PC run: modlock join "+o.code)
		o.codeShown = true
	}
}

// hostFeed keeps every player's interface from the session's first event, so
// a daemon that starts hosting later gets the whole state, then each change
// and each chat line.
type hostFeed struct {
	// mu guards the fields below.
	mu sync.Mutex
	// screens holds every player's interface.
	screens *session.Screens
	// queue holds the events the daemon has yet to receive, once sending.
	queue []*control.HostEvent
	// sending is true once send took the state.
	sending bool
	// wake holds a token while queue may have events.
	wake chan struct{}
}

// newHostFeed returns a feed with no players.
func newHostFeed() *hostFeed {
	return &hostFeed{screens: session.NewScreens(0), wake: make(chan struct{}, 1)}
}

// observe follows the players, interfaces and chat in event.
func (f *hostFeed) observe(event *modcli.Event) {
	// Take only the events that change what a guest sees.
	host := event.GetHost()
	if host.GetJoined() == nil && host.GetLeft() == nil && host.GetUi() == nil && host.GetChat() == nil {
		return
	}

	// Keep the state, and queue the change once sending.
	f.mu.Lock()
	defer f.mu.Unlock()
	f.screens.Handle(host)
	if f.sending {
		f.queue = append(f.queue, host)
		f.signal()
	}
}

// signal wakes send. Callers hold mu.
func (f *hostFeed) signal() {
	select {
	case f.wake <- struct{}{}:
	default:
	}
}

// send sends the daemon the whole state, then each change, until ctx ends or
// the stream fails.
func (f *hostFeed) send(ctx context.Context, stream peernet.SRPCPeerNet_HostClient) {
	// Start from the state so far.
	f.mu.Lock()
	f.queue, f.sending = f.screens.Snapshot(), true
	f.signal()
	f.mu.Unlock()
	defer f.stop()

	// Send each batch as it arrives.
	for {
		select {
		case <-ctx.Done():
			return
		case <-f.wake:
		}
		f.mu.Lock()
		batch := f.queue
		f.queue = nil
		f.mu.Unlock()
		for _, event := range batch {
			if stream.Send(&peernet.HostInput{Body: &peernet.HostInput_Server{Server: event}}) != nil {
				return
			}
		}
	}
}

// stop drops the queue once nothing sends it.
func (f *hostFeed) stop() {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.queue, f.sending = nil, false
}

// joinCommand joins a session another PC hosts and launches Deadlock into it.
func joinCommand(out *printer) *cli.Command {
	return &cli.Command{
		Name:      "join",
		Usage:     "join a session another PC hosts with modlock host, by its join code or lobby link, and launch Deadlock into it",
		ArgsUsage: "CODE|LINK",
		Flags: append([]cli.Flag{
			&cli.BoolFlag{Name: "no-game", Usage: "only serve the session; join it yourself with the console line"},
			&cli.StringFlag{Name: "origin", Value: modpublish.DefaultOrigin, Usage: "the Hyperline service whose lobby links join takes"},
		}, interfaceFlags...),
		Action: func(c *cli.Context) error {
			// Read the code, or the lobby link that names it.
			arg := c.Args().First()
			if arg == "" {
				return errors.New("pass the join code or lobby link modlock host printed")
			}
			out.brief = true
			out.header("join", arg)
			code, err := joinCode(c.Context, arg, c.String("origin"))
			if err != nil {
				return err
			}

			// Ask the daemon to join, streaming this player's screen.
			steam, steamErr := session.FindSteam()
			var account uint64
			if steamErr == nil && c.Uint("ui-port") != 0 {
				account = steam.Account()
			}
			client, err := connectDaemon(c.Context)
			if err != nil {
				return err
			}
			defer client.Close()
			stream, err := peernet.NewSRPCPeerNetClient(client.RPC()).Join(c.Context, &peernet.JoinRequest{Code: code, SteamId: account})
			if err != nil {
				return err
			}

			// Take the mods' content, then mount it with this player's
			// interfaces and launch the game at the endpoint, and hold
			// until it leaves.
			out.mark("…", dim, "joining…")
			mirror := session.NewMirror()
			content := fstest.MapFS{}
			for {
				event, err := stream.Recv()
				switch {
				case c.Context.Err() != nil:
					return nil
				case errors.Is(err, io.EOF):
					out.mark("←", dim, "left")
					return nil
				case err != nil:
					return err
				}
				switch body := event.GetBody().(type) {
				case *peernet.JoinEvent_Content:
					file := content[body.Content.GetPath()]
					if file == nil {
						file = &fstest.MapFile{Mode: 0o644}
						content[body.Content.GetPath()] = file
					}
					file.Data = append(file.Data, body.Content.GetData()...)
				case *peernet.JoinEvent_Address:
					if steamErr == nil {
						defer mountGuest(c, out, steam, mirror, content)()
					}
					launchGuest(c, out, body.Address)
				case *peernet.JoinEvent_Arrived:
					out.mark("✓", green, "joined")
				case *peernet.JoinEvent_Screen:
					mirror.Apply(body.Screen.GetChanges(), body.Screen.GetReset_())
				case *peernet.JoinEvent_Chat:
					out.host(&control.HostEvent{Body: &control.HostEvent_Chat{Chat: body.Chat}})
				}
			}
		},
	}
}

// joinCode returns the join code arg names: a code modlock host printed, or
// the link of a dev lobby at origin, whose host session the service names to
// a member. The saved sign-in goes only to origin.
func joinCode(ctx context.Context, arg, origin string) (string, error) {
	// Take a join code as it is.
	link, err := url.Parse(arg)
	if err != nil || link.Scheme == "" {
		return arg, nil
	}
	lobby := link.Query().Get("lobby")
	if link.Scheme+"://"+link.Host != origin || lobby == "" {
		return "", errors.Errorf("a lobby link is a lobby page on %s", origin)
	}

	// Ask the service for the lobby's host session.
	service, err := hyperlineService(origin)
	if err != nil {
		return "", err
	}
	host, err := service.JoinDevLobby(ctx, lobby)
	if err != nil {
		return "", err
	}
	id, err := peer.IDB58Decode(host.GetPeerId())
	if err != nil {
		return "", errors.Wrap(err, "read the lobby's host")
	}
	return peernet.JoinCode{Host: id, Session: host.GetSecret()}.String(), nil
}

// launchGuest launches Deadlock into address, or names the console line that
// joins it when the guest joins by hand.
func launchGuest(c *cli.Context, out *printer, address string) {
	// Steam cannot connect a game that is already running, so name the line.
	if c.Bool("no-game") {
		out.mark("●", green, "in a running game's console, enter: connect "+address)
		return
	}

	// Launch the game, which connects once it starts.
	steam, err := session.FindSteam()
	if err == nil {
		err = steam.LaunchDeadlock(c.Context, "-console", "-condebug", "+connect", address)
	}
	if err != nil {
		out.mark("!", yellow, err.Error()+"; in a running game's console, enter: connect "+address)
	}
}

// mountGuest mounts the host's mods' content in the player's game and serves
// mirror to it as their interfaces, and returns the call that stops both.
// With the interfaces off it mounts only the content. A game already running
// loads neither until it restarts.
func mountGuest(c *cli.Context, out *printer, steam *session.Steam, mirror *session.Mirror, content fstest.MapFS) func() {
	gameDir, err := steam.Deadlock()
	if err != nil {
		out.mark("!", yellow, "mod content and interfaces are off: "+err.Error())
		return func() {}
	}
	iface := session.Interface{GameDir: gameDir, Content: content, Screen: mirror.Screen, Events: out.host}
	if port := c.Uint("ui-port"); port != 0 {
		renderer, err := readRenderer(c.Context)
		if err != nil {
			out.mark("!", yellow, "mod interfaces are off: "+err.Error())
		} else {
			iface.Address, iface.Relay, iface.Renderer = "127.0.0.1:"+strconv.Itoa(int(port)), c.String("ui-relay"), renderer
		}
	}
	return session.StartInterface(iface).Stop
}
