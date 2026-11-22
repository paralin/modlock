package main

import (
	"context"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"strconv"
	"sync"
	"time"

	"github.com/aperturerobotics/cli"
	"github.com/paralin/modlock/js"
	"github.com/paralin/modlock/session"
	"github.com/paralin/modlock/source2/vpk"
	"github.com/paralin/modlock/studio"
)

// studioCommand serves Modlock Studio on a loopback port until interrupted.
func studioCommand(out *printer) *cli.Command {
	return &cli.Command{
		Name:  "studio",
		Usage: "open Modlock Studio in a browser to browse the game's assets",
		Description: "Modlock Studio reads the game's models, materials, textures and sounds from your own\n" +
			"Deadlock install, found through Steam or DEADLOCK_DIR, and previews them in the page.",
		Flags: []cli.Flag{
			&cli.IntFlag{Name: "port", Value: 7777, Usage: "the loopback `PORT` to serve on, or a free one when taken"},
		},
		Subcommands: []*cli.Command{studioRenderCommand(out)},
		Action: func(c *cli.Context) error {
			out.header("studio", "")

			// Open the game's archive; without it the page still serves and
			// says why.
			page, assets, err := openStudio(c.Context, out)
			if err != nil {
				return err
			}

			// Serve the page until interrupted.
			listener, err := listenLoopback(c.Int("port"))
			if err != nil {
				return err
			}
			out.field("studio", "http://"+listener.Addr().String()+"/")
			return serveStudio(c.Context, listener, studio.Handler(page, assets, nil))
		},
	}
}

// openStudio builds or unpacks the page and opens the game's archive,
// printing where the archive is or why it is unavailable, which the
// returned Assets also reports through Err.
func openStudio(ctx context.Context, out *printer) (string, *studio.Assets, error) {
	// Build or unpack the page with the rest of the library.
	library, err := js.Library(ctx)
	if err != nil {
		return "", nil, err
	}
	page := filepath.Join(library, js.Studio)

	// Open the game's archive.
	dir, err := deadlockDir()
	assets := studio.Unavailable(err)
	if err == nil {
		assets = studio.OpenAssets(filepath.Join(dir, "game", "citadel"))
		err = assets.Err()
	}
	if err != nil {
		out.mark("!", yellow, "no game assets: "+err.Error())
	} else {
		out.field("game", dir)
	}
	return page, assets, nil
}

// listenLoopback listens on a loopback port, or on a free one when another
// program holds it.
func listenLoopback(port int) (net.Listener, error) {
	listener, err := net.Listen("tcp", net.JoinHostPort("127.0.0.1", strconv.Itoa(port)))
	if err != nil {
		listener, err = net.Listen("tcp", "127.0.0.1:0")
	}
	return listener, err
}

// serveStudio serves handler on listener until ctx ends.
func serveStudio(ctx context.Context, listener net.Listener, handler http.Handler) error {
	server := &http.Server{Handler: handler, ReadHeaderTimeout: 10 * time.Second}
	go func() {
		<-ctx.Done()
		_ = server.Close()
	}()
	if err := server.Serve(listener); err != http.ErrServerClosed {
		return err
	}
	return nil
}

// stockFiles returns a reader of the installed game's archive, or nil when no
// Deadlock install is found. The archive opens once per process.
var stockFiles = sync.OnceValue(func() func(name string) ([]byte, error) {
	// Find the install and open its archive.
	dir, err := deadlockDir()
	if err != nil {
		return nil
	}
	archive, err := vpk.OpenArchive(filepath.Join(dir, "game", "citadel", "pak01_dir.vpk"))
	if err != nil {
		return nil
	}
	return archive.ReadFile
})

// deadlockDir returns the Deadlock install directory: DEADLOCK_DIR, or the
// one in the Steam libraries.
func deadlockDir() (string, error) {
	if dir := os.Getenv("DEADLOCK_DIR"); dir != "" {
		return dir, nil
	}
	steam, err := session.FindSteam()
	if err != nil {
		return "", err
	}
	return steam.Deadlock()
}
