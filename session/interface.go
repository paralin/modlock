package session

import (
	"net"
	"net/http"
	"net/url"
	"testing/fstest"

	"github.com/paralin/modlock/devmount"
	"github.com/paralin/modlock/src/host_app/control"
	"github.com/pkg/errors"
)

// Interface configures serving a player's mod interfaces and content to
// their game.
type Interface struct {
	// Address is the loopback address the relay page reads from, or empty to
	// serve no interfaces.
	Address string
	// Relay is the relay page's origin.
	Relay string
	// Renderer is the built renderer; nil leaves the game without it, so the
	// interfaces show only in the browser preview.
	Renderer *Renderer
	// Content is the mods' content addons the game loads, as ReadContent
	// returns them.
	Content fstest.MapFS
	// GameDir is the Deadlock installation the renderer mounts in.
	GameDir string
	// Screen is the player's screen.
	Screen ScreenSource
	// Events receives a failure that turns part of the interfaces off.
	Events func(*control.HostEvent)
}

// Served is a player's interfaces and content, started by StartInterface.
type Served struct {
	config Interface
	// mounted is the package in the game, or nil when none mounted.
	mounted *mount
	// server serves the screen, or is nil when it does not.
	server *http.Server
}

// StartInterface mounts the renderer and the mods' content in the game and
// serves the screen to the relay page. Each part is optional: a game that
// cannot take the package or another program on the address reports a
// failure and leaves the session running. Start it before the server and the
// game, which load the package only when they start.
func StartInterface(config Interface) *Served {
	// Mount the renderer and the content, so the game draws the screen and
	// the server and the game load the mods' resources.
	served := &Served{config: config}
	mounted, err := mountRenderer(config.GameDir, config.Renderer, config.bridge(), config.Content)
	if err != nil {
		config.fail(errors.Wrap(err, "the game shows no mod interfaces or content"))
	}
	served.mounted = mounted
	if config.Address == "" {
		return served
	}

	// Serve the screen. Without it the renderer has nothing to draw.
	listener, err := net.Listen("tcp", config.Address)
	if err != nil {
		config.fail(errors.Wrap(err, "mod interfaces are off"))
		return served
	}
	served.server = &http.Server{Handler: NewRelay(config.Relay, config.Screen)}
	go func() { _ = served.server.Serve(listener) }()
	return served
}

// AddContent carries the content of the mod rebuilt at mod into the mounted
// package, so the server and a running game load its changed addon without a
// restart. It reports a failure, and runs one call at a time.
func (s *Served) AddContent(mod string) {
	content, err := ReadContent([]string{mod})
	switch {
	case err != nil:
		s.config.fail(errors.Wrap(err, "the game keeps the mod's earlier content"))
	case len(content) == 0:
	case s.mounted == nil:
		s.config.fail(errors.New("the game has no mod package to add the changed content to; restart the session to load it"))
	default:
		if err := s.mounted.Add(content); err != nil {
			s.config.fail(errors.Wrap(err, "the game keeps the mod's earlier content"))
		}
	}
}

// Stop stops serving the screen and unmounts the package. A block left in
// gameinfo.gi outranks every package the game loads after the session, so it
// reports a failure to remove it.
func (s *Served) Stop() {
	if s.server != nil {
		_ = s.server.Close()
	}
	if s.mounted == nil {
		return
	}
	if err := s.mounted.Close(); err != nil {
		s.config.fail(errors.Wrapf(err, "remove the %s block from gameinfo.gi; the next session replaces it", devmount.Name))
	}
}

// bridge returns the URL of the relay page that reads the screen from
// Address, or empty when no screen is served. The page takes the port in its
// query, so the renderer reaches a server on any port.
func (c Interface) bridge() string {
	_, port, err := net.SplitHostPort(c.Address)
	if err != nil {
		return ""
	}
	return c.Relay + BridgePath + "?" + url.Values{"port": {port}}.Encode()
}

// fail reports err as a failure event.
func (c Interface) fail(err error) {
	c.Events(&control.HostEvent{Body: &control.HostEvent_Failed{Failed: &control.ModFailed{Error: err.Error()}}})
}
