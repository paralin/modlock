package main

import (
	"net"
	"net/http"
	"os"
	"path/filepath"
	"strconv"
	"time"

	"github.com/aperturerobotics/cli"
	"github.com/paralin/modlock/js"
	"github.com/paralin/modlock/session"
	"github.com/paralin/modlock/studio"
)

// studioCommand serves Modlock Studio on a loopback port until interrupted.
func studioCommand(out *printer) *cli.Command {
	return &cli.Command{
		Name:  "studio",
		Usage: "open Modlock Studio in a browser to browse the game's assets",
		Description: "Modlock Studio reads the game's models, materials and textures from your own\n" +
			"Deadlock install, found through Steam or DEADLOCK_DIR, and draws them in the page.",
		Flags: []cli.Flag{
			&cli.IntFlag{Name: "port", Value: 7777, Usage: "the loopback `PORT` to serve on, or a free one when taken"},
		},
		Action: func(c *cli.Context) error {
			out.header("studio", "")

			// Build or unpack the page with the rest of the library.
			library, err := js.Library(c.Context)
			if err != nil {
				return err
			}

			// Open the game's archive; without it the page still serves and
			// says why.
			dir, err := deadlockDir()
			assets := studio.Unavailable(err)
			if err == nil {
				assets = studio.OpenAssets(filepath.Join(dir, "game", "citadel", "pak01_dir.vpk"))
				err = assets.Err()
			}
			if err != nil {
				out.mark("!", yellow, "no game assets: "+err.Error())
			} else {
				out.field("game", dir)
			}

			// Listen on loopback, on a free port when another program holds this one.
			listener, err := net.Listen("tcp", net.JoinHostPort("127.0.0.1", strconv.Itoa(c.Int("port"))))
			if err != nil {
				listener, err = net.Listen("tcp", "127.0.0.1:0")
			}
			if err != nil {
				return err
			}
			out.field("studio", "http://"+listener.Addr().String()+"/")
			handler := studio.Handler(filepath.Join(library, js.Studio), assets)
			server := &http.Server{Handler: handler, ReadHeaderTimeout: 10 * time.Second}
			go func() {
				<-c.Context.Done()
				_ = server.Close()
			}()
			if err := server.Serve(listener); err != http.ErrServerClosed {
				return err
			}
			return nil
		},
	}
}

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
