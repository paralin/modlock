package session

import (
	"context"
	"runtime"
	"strconv"
	"sync"
	"sync/atomic"

	"github.com/paralin/modlock/src/host_app/control"
	"github.com/paralin/modlock/src/wasm/players"
	"github.com/pkg/errors"
)

// DefaultPort is the server's default UDP port.
const DefaultPort = 27067

// DefaultInterfacePort is the loopback port the relay page reads the mods'
// interfaces from when its URL names none.
const DefaultInterfacePort = 4320

// DefaultRelay hosts the relay page the game reads the mods' interfaces
// through.
const DefaultRelay = "https://hyperline.gg"

// BridgePath is the relay page's path under its origin. The page reads the
// screen from the loopback port its query names as port, or from
// DefaultInterfacePort.
const BridgePath = "/app/relay/bridge.html"

// Session is a running play session: the host with the mods and, once the
// server is ready, the Deadlock client joined to it.
type Session struct {
	// Executable is the modlock-host.exe the session runs: MODLOCK_HOST, or
	// the downloaded release.
	Executable string
	// control is the link to the host.
	control *Control
	// host is the host process.
	host *Host
	// served is the mods' interfaces and content in the player's game.
	served *Served
	// stopInterface unmounts the mods' interfaces and content from the
	// player's game and stops serving the interfaces; it runs once.
	stopInterface func()
	// stopOnDone ends the session when Start's context ends.
	stopOnDone func() bool
	// controlled is closed when the control link ends.
	controlled chan struct{}
}

// Start starts the host with the mods and serves its control link. It
// returns an error marked ErrNoGame when this computer cannot run the game.
func Start(ctx context.Context, config Config) (*Session, error) {
	// Find a system that runs the host, Steam and the game.
	if runtime.GOOS != "windows" && runtime.GOOS != "linux" {
		return nil, noGame(errors.New("Deadlock servers run on Windows, or on Linux through Proton"))
	}
	steam, err := FindSteam()
	if err != nil {
		return nil, noGame(err)
	}
	gameDir, err := steam.Deadlock()
	if err != nil {
		return nil, noGame(err)
	}

	// Find the host.
	executable, err := FindHost(ctx, config.Version)
	if err != nil {
		return nil, err
	}

	// Listen for the host before it starts, so it can dial at once.
	link, err := NewControl()
	if err != nil {
		return nil, err
	}
	// The local players are the mod's own developers, so give every human
	// both roles.
	local := &players.RolesTable{Humans: []players.ModRole{players.ModRole_MOD_ROLE_DEVELOPER, players.ModRole_MOD_ROLE_TESTER}}
	if err := link.SetRoles(ctx, local); err != nil {
		_ = link.Close()
		return nil, err
	}
	// Mount the mods' interfaces and content before the host starts, so the
	// server loads the content too.
	screens := NewScreens(steam.Account())
	iface := Interface{GameDir: gameDir, Screen: screens.Screen, Events: config.Events}
	if iface.Content, err = ReadContent(config.Mods); err != nil {
		iface.fail(errors.Wrap(err, "the game shows no mod content"))
	}
	if config.Interface != "" {
		iface.Address, iface.Relay, iface.Renderer = config.Interface, config.Relay, config.Renderer
	}
	served := StartInterface(iface)
	stopInterface := sync.OnceFunc(served.Stop)
	// The host outlives ctx until Stop has unmounted, since a game joined to
	// the server reloads its search paths when the server stops, holding
	// gameinfo.gi past any unmount that follows.
	host, err := StartHost(context.WithoutCancel(ctx), HostConfig{
		Executable: executable,
		GameDir:    gameDir,
		Mods:       config.Mods,
		Control:    link.Address(),
		Port:       config.Port,
		Map:        config.Map,
		Args:       config.Args,
		Settings:   config.Settings,
		Files:      config.Files,
		Steam:      steam,
		Output:     config.HostOutput,
	})
	if err != nil {
		stopInterface()
		_ = link.Close()
		return nil, err
	}

	session := &Session{Executable: executable, control: link, host: host, controlled: make(chan struct{}), served: served, stopInterface: stopInterface}
	session.stopOnDone = context.AfterFunc(ctx, session.Stop)
	// Pass events on, launching the client at the first ready server.
	var launched atomic.Bool
	handle := func(event *control.HostEvent) {
		screens.Handle(event)
		if event.GetReady() != nil && config.Launch && !launched.Swap(true) {
			address := "127.0.0.1:" + strconv.Itoa(int(config.Port))
			if err := steam.LaunchDeadlock(ctx, "-console", "-condebug", "+connect", address); err != nil {
				event = &control.HostEvent{Body: &control.HostEvent_Failed{Failed: &control.ModFailed{
					Error: errors.Wrap(err, "launch Deadlock").Error(),
				}}}
			}
		}
		config.Events(event)
	}
	go func() {
		defer close(session.controlled)
		_ = link.Run(ctx, handle)
	}()
	return session, nil
}

// Reload asks the host to replace the running mod built at path, first
// adding the mod's changed content to the mounted package, so the reloaded
// mod finds the content its rebuilt addon names.
func (s *Session) Reload(ctx context.Context, path string) error {
	s.served.AddContent(path)
	return s.control.Reload(ctx, path)
}

// Wait waits for the host to exit and its last events to arrive.
func (s *Session) Wait() error {
	// Wait for the host, then close the link and the interfaces it fed.
	err := s.host.Wait()
	s.stopOnDone()
	_ = s.control.Close()
	s.stopInterface()
	<-s.controlled
	return err
}

// Stop unmounts the mods from the player's game, then ends the host. The
// unmount comes first because the game, once its server stops, holds
// gameinfo.gi while it reloads its search paths; the running server and game
// keep the package they loaded.
func (s *Session) Stop() {
	s.stopInterface()
	s.host.Stop()
}
