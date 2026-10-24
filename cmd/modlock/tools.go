package main

import (
	"net"
	"net/http"
	"os"
	"path/filepath"
	"strconv"
	"time"

	"github.com/aperturerobotics/cli"
	"github.com/paralin/modlock/session"
	"github.com/paralin/modlock/tools"
)

// toolsCommand serves the tools page on a loopback port until interrupted.
func toolsCommand(out *printer) *cli.Command {
	return &cli.Command{
		Name:  "tools",
		Usage: "open Modlock's tools in a browser, such as the asset browser",
		Description: "The asset browser reads the game's models, materials and textures from your\n" +
			"own Deadlock install, found through Steam or DEADLOCK_DIR, and draws them in the page.",
		Flags: []cli.Flag{
			&cli.IntFlag{Name: "port", Value: 7777, Usage: "the loopback `PORT` to serve on, or a free one when taken"},
		},
		Action: func(c *cli.Context) error {
			out.header("tools", "")

			// Find the game's archive; the tools without it still serve.
			var assets *tools.Assets
			dir, err := deadlockDir()
			if err == nil {
				assets, err = tools.OpenAssets(filepath.Join(dir, "game", "citadel", "pak01_dir.vpk"))
			}
			if err != nil {
				out.mark("!", yellow, "the asset browser is off: "+err.Error())
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
			out.field("tools", "http://"+listener.Addr().String()+"/")
			server := &http.Server{Handler: tools.Handler(assets), ReadHeaderTimeout: 10 * time.Second}
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
