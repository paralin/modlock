package main

import (
	"context"
	"crypto/sha256"
	"net"
	"net/http"
	"path/filepath"
	"slices"
	"strconv"
	"sync/atomic"
	"time"

	"github.com/aperturerobotics/cli"
	modcli "github.com/paralin/modlock/cli"
	"github.com/paralin/modlock/js"
	"github.com/paralin/modlock/peernet"
	"github.com/paralin/modlock/preview"
	"github.com/paralin/modlock/sandbox"
	"github.com/pkg/errors"
)

// previewPort is the default loopback port of the preview page. Hyperline's
// desktop holds 4321 while it runs, so a creator can run both.
const previewPort = 4322

// devCommand runs a mod in a local server and, with --watch, reloads it on
// each save. It serves the preview page, which draws the mod's interface.
func devCommand(out *printer) *cli.Command {
	return &cli.Command{
		Name:      "dev",
		Usage:     "build the mod and run it in a local server, or in the sandbox without the game",
		ArgsUsage: "[DIRECTORY]",
		Flags: append(
			slices.Clone(sessionFlags),
			&cli.BoolFlag{
				Name:  "watch",
				Usage: "rebuild the mod and reload it on each save",
			},
			releaseFlag,
			&cli.UintFlag{
				Name:  "preview-port",
				Value: previewPort,
				Usage: "the loopback port of the page that draws the mod's interface; 0 turns it off",
			},
		),
		Action: func(c *cli.Context) error {
			return runDev(c, out, "dev", false)
		},
	}
}

// runDev runs command, which builds the mod and runs it in a local server,
// rebuilding and reloading it on each save when watching. A host session
// always watches and serves the server to guests.
func runDev(c *cli.Context, out *printer, command string, host bool) error {
	// Open the project.
	opened, err := openProject(c)
	if err != nil {
		return err
	}
	out.header(command, opened.Manifest.GetSlug())

	// Serve the preview page, which takes the player's input once
	// the mods run in the sandbox.
	watch := host || c.Bool("watch")
	var box atomic.Pointer[sandbox.Sandbox]
	if port := c.Uint("preview-port"); port != 0 {
		servePreview(c.Context, out, int(port), opened.Output(), watch, func(ctx context.Context, in *modcli.Input) string {
			if box := box.Load(); box != nil {
				return deliver(ctx, box, in)
			}
			return "the preview passes input to the mods only in the sandbox, once they run"
		})
	}

	// Keep every player's interface from the first event, for guests.
	var feed *hostFeed
	if host {
		feed = newHostFeed()
		out.tap(feed.observe)
	}

	// Build now and, with --watch, after each save. The first good
	// build starts the server and each later one reloads the mod; a
	// failed build keeps the running one, and without --watch ends the
	// command. A build that made the same mod as the running one, after a
	// save outside its inputs such as a note, leaves the game alone. Watch
	// calls rebuild on one goroutine.
	var running server
	var loaded [sha256.Size]byte
	started := make(chan error, 1)
	rebuild := func() {
		// Keep the running mod when the build fails.
		if err := out.build(c.Context, opened, buildMode(c)); err != nil {
			if !watch {
				started <- errors.Wrap(err, "the build failed; fix it and run again, or pass --watch to rebuild on each save")
				return
			}
			out.mark("✗", red, "the build failed; fix it and save to try again")
			return
		}
		built, err := opened.Fingerprint()
		if err != nil {
			out.mark("!", yellow, err.Error())
		}

		// Reload a running server when the mod changed, or start the first one.
		if running != nil {
			if err == nil && built == loaded {
				out.mark("·", dim, "the mod did not change; the game keeps running it")
				return
			}
			loaded = built
			_ = running.Reload(c.Context, opened.Output())
			return
		}
		loaded = built
		running, err = startSession(c, out, []string{opened.Output()}, filepath.Join(opened.Output(), "server.log"))
		if sandboxed, ok := running.(*sandbox.Sandbox); ok {
			box.Store(sandboxed)
		}
		started <- err
	}

	// Build once, then after each save when watching.
	go func() {
		rebuild()
		if !watch {
			return
		}
		err := opened.Watch(c.Context, func() {
			out.mark("…", dim, "rebuilding "+opened.Manifest.GetSlug())
			rebuild()
		})
		if err != nil && c.Context.Err() == nil {
			out.mark("!", yellow, errors.Wrap(err, "stopped watching").Error())
		}
	}()

	// Run the session once it starts.
	select {
	case <-c.Context.Done():
		return nil
	case err := <-started:
		if err != nil {
			return err
		}
	}
	defer running.Stop()
	if host {
		var lobby *peernet.LobbyRequest
		if !c.Bool("no-lobby") {
			lobby = &peernet.LobbyRequest{ModName: opened.Manifest.GetName(), Version: opened.Manifest.GetVersion(), Slug: opened.Manifest.GetSlug()}
		}
		go hostSession(c, out, running, feed, lobby, []string{opened.Output()})
	}
	return waitSession(c, out, running)
}

// servePreview serves the preview page on the loopback port until ctx ends,
// for the mod built in built that rebuilds on each save when watch is true,
// passing its input to input. Another program on the port moves the page to a
// free one. The page is optional: when it cannot start, the mod runs without
// it.
func servePreview(ctx context.Context, out *printer, port int, built string, watch bool, input preview.Input) {
	// Find the page's files in the library.
	library, err := js.Library(ctx)
	if err != nil {
		out.mark("!", yellow, "the preview page is off: "+err.Error())
		return
	}
	page := preview.New(filepath.Join(library, js.Preview), built, watch, input)

	// Listen on loopback, on a free port when another program holds this one,
	// and follow the command line's events.
	listener, err := net.Listen("tcp", net.JoinHostPort("127.0.0.1", strconv.Itoa(port)))
	if err != nil {
		listener, err = net.Listen("tcp", "127.0.0.1:0")
	}
	if err != nil {
		out.mark("!", yellow, "the preview page is off: "+err.Error())
		return
	}
	out.tap(page.Observe)
	server := &http.Server{Handler: page, ReadHeaderTimeout: 10 * time.Second}
	context.AfterFunc(ctx, func() { _ = server.Close() })
	go func() { _ = server.Serve(listener) }()
	out.field("preview", "http://"+listener.Addr().String()+"/")
}
