package session

import (
	"context"
	"io"
	"net"
	"net/http"
	"os"
	"strconv"
	"sync/atomic"

	"github.com/paralin/modlock/proto/modlock/control"
	"github.com/pkg/errors"
)

// DefaultPort is the server's default UDP port.
const DefaultPort = 27067

// DefaultInterfacePort is the loopback port the relay page reads the mods'
// interfaces from.
const DefaultInterfacePort = 4320

// DefaultRelay hosts the relay page the game reads the mods' interfaces
// through.
const DefaultRelay = "https://hyperline.gg"

// Config describes a play session.
type Config struct {
	// Version is the command line's release, whose host the session runs
	// unless MODLOCK_HOST names one.
	Version string
	// Mods lists the built mods to load.
	Mods []string
	// Port is the server's UDP port.
	Port uint16
	// Map is the map the server starts, or empty for the default.
	Map string
	// Args are passed to each mod's start handlers.
	Args []string
	// Launch starts the Deadlock client once the server is ready.
	Launch bool
	// HostOutput receives the host's console output.
	HostOutput io.Writer
	// Interface is the loopback address the player's game reads the mods'
	// interfaces from, or empty to serve none.
	Interface string
	// Relay is the origin of the https page the game reads them through.
	Relay string
	// Events receives each host event, on one goroutine.
	Events func(*control.HostEvent)
}

// Session is a running play session: the host with the mods and, once the
// server is ready, the Deadlock client joined to it.
type Session struct {
	// control is the link to the host.
	control *Control
	// host is the host process.
	host *Host
	// screens serves the mods' interfaces to the player's game, if it runs.
	screens *http.Server
	// controlled is closed when the control link ends.
	controlled chan struct{}
}

// Start starts the host with the mods and serves its control link.
func Start(ctx context.Context, config Config) (*Session, error) {
	// Find Steam, the game and the host.
	steam, err := FindSteam()
	if err != nil {
		return nil, err
	}
	gameDir := os.Getenv("DEADLOCK_DIR")
	if gameDir == "" {
		if gameDir, err = steam.Deadlock(); err != nil {
			return nil, err
		}
	}
	executable, err := FindHost(ctx, config.Version)
	if err != nil {
		return nil, err
	}

	// Listen for the host before it starts, so it can dial at once.
	link, err := NewControl()
	if err != nil {
		return nil, err
	}
	host, err := StartHost(ctx, HostConfig{
		Executable: executable,
		GameDir:    gameDir,
		Mods:       config.Mods,
		Control:    link.Address(),
		Port:       config.Port,
		Map:        config.Map,
		Args:       config.Args,
		Steam:      steam,
		Output:     config.HostOutput,
	})
	if err != nil {
		_ = link.Close()
		return nil, err
	}

	// Serve the mods' interfaces to the game. Another program on the port
	// leaves them off without stopping the session.
	session := &Session{control: link, host: host, controlled: make(chan struct{})}
	screens := NewScreens(steam.Account(), config.Relay)
	if config.Interface != "" {
		if listener, err := net.Listen("tcp", config.Interface); err != nil {
			config.Events(&control.HostEvent{Body: &control.HostEvent_Failed{Failed: &control.ModFailed{
				Error: errors.Wrap(err, "mod interfaces are off").Error(),
			}}})
		} else {
			session.screens = &http.Server{Handler: screens}
			go func() { _ = session.screens.Serve(listener) }()
		}
	}

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

// Reload asks the host to replace the running mod built at path.
func (s *Session) Reload(ctx context.Context, path string) error {
	return s.control.Reload(ctx, path)
}

// Wait waits for the host to exit and its last events to arrive.
func (s *Session) Wait() error {
	err := s.host.Wait()
	_ = s.control.Close()
	if s.screens != nil {
		_ = s.screens.Close()
	}
	<-s.controlled
	return err
}

// Stop ends the host.
func (s *Session) Stop() {
	s.host.Stop()
}
