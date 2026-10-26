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

// StartInterface mounts the renderer and the mods' content in the game and
// serves the screen to the relay page, and returns the call that stops both.
// Each part is optional: a game that cannot take the package or another
// program on the address reports a failure and leaves the session running.
// Start it before the server and the game, which load the package only when
// they start.
func StartInterface(config Interface) (stop func()) {
	// Mount the renderer and the content, so the game draws the screen and
	// the server and the game load the mods' resources.
	// A block left in gameinfo.gi outranks every package the game loads after
	// the session, so report a failure to remove it.
	unmount := func() {}
	if mounted, err := mountRenderer(config.GameDir, config.Renderer, config.bridge(), config.Content); err != nil {
		config.fail(errors.Wrap(err, "the game shows no mod interfaces or content"))
	} else {
		unmount = func() {
			if err := mounted(); err != nil {
				config.fail(errors.Wrapf(err, "remove the %s block from gameinfo.gi; the next session replaces it", devmount.Name))
			}
		}
	}
	if config.Address == "" {
		return unmount
	}

	// Serve the screen. Without it the renderer has nothing to draw.
	listener, err := net.Listen("tcp", config.Address)
	if err != nil {
		config.fail(errors.Wrap(err, "mod interfaces are off"))
		return unmount
	}
	server := &http.Server{Handler: NewRelay(config.Relay, config.Screen)}
	go func() { _ = server.Serve(listener) }()
	return func() {
		_ = server.Close()
		unmount()
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
