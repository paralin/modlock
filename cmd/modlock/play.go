package main

import (
	"context"
	"io"
	"os"
	"path/filepath"
	"strconv"

	"github.com/aperturerobotics/cli"
	"github.com/paralin/modlock/project"
	"github.com/paralin/modlock/session"
	"github.com/pkg/errors"
)

// sessionFlags configure the server and client of a session.
var sessionFlags = []cli.Flag{
	&cli.UintFlag{Name: "port", Value: session.DefaultPort, Usage: "the server's UDP port"},
	&cli.BoolFlag{Name: "no-game", Usage: "run only the server; join it yourself"},
	&cli.StringSliceFlag{Name: "arg", Usage: "pass an argument to the mods' start handlers; repeat for several"},
	&cli.UintFlag{
		Name:  "ui-port",
		Value: session.DefaultInterfacePort,
		Usage: "the loopback port the game reads the mods' interfaces from; 0 turns them off",
	},
	&cli.StringFlag{
		Name:  "ui-relay",
		Value: session.DefaultRelay,
		Usage: "the origin of the https page the game reads the mods' interfaces through",
	},
}

// playCommand runs built mods in a local server.
func playCommand(out *printer) *cli.Command {
	return &cli.Command{
		Name:      "play",
		Usage:     "run built mods in a local server and join it",
		ArgsUsage: "[MOD...]",
		Flags:     sessionFlags,
		Action: func(c *cli.Context) error {
			// Take each mod as a built mod or a project's build.
			paths := c.Args().Slice()
			if len(paths) == 0 {
				paths = []string{"."}
			}
			mods := make([]string, 0, len(paths))
			for _, path := range paths {
				dir, err := filepath.Abs(path)
				if err != nil {
					return err
				}
				if opened, err := project.Open(dir); err == nil {
					dir = opened.Output()
				}
				if _, err := os.Stat(filepath.Join(dir, project.ManifestFile)); err != nil {
					return errors.Errorf("%s is not a built mod; run modlock build first", path)
				}
				mods = append(mods, dir)
			}

			// Run the session until the server stops.
			cache, err := os.UserCacheDir()
			if err != nil {
				return err
			}
			running, err := startSession(c, out, mods, filepath.Join(cache, "modlock", "server.log"))
			if err != nil {
				return err
			}
			defer running.Stop()
			return waitSession(c, out, running)
		},
	}
}

// startSession starts a session with mods on the first map a mod names,
// writing the server's console to logPath.
func startSession(c *cli.Context, out *printer, mods []string, logPath string) (*session.Session, error) {
	// Load the first map a mod names.
	var mapName string
	for _, mod := range mods {
		manifest, err := project.ReadManifest(mod)
		if err != nil {
			return nil, err
		}
		if mapName = manifest.GetMap(); mapName != "" {
			break
		}
	}

	// Keep the server's console in a file; the mods' own lines arrive as
	// events.
	if err := os.MkdirAll(filepath.Dir(logPath), 0o755); err != nil {
		return nil, err
	}
	log, err := os.Create(logPath)
	if err != nil {
		return nil, err
	}
	context.AfterFunc(c.Context, func() { _ = log.Close() })

	// Start the server, which launches the client once it is ready. Steam
	// cannot connect a game that is already running, so name the console
	// line that joins.
	out.note("starting the server; its console is in", logPath)
	out.note("to join from a running game, enter in its console: connect 127.0.0.1:" + strconv.Itoa(int(c.Uint("port"))))

	// Serve the mods' interfaces unless they are off, and report the
	// server's events.
	var ui string
	if port := c.Uint("ui-port"); port != 0 {
		ui = "127.0.0.1:" + strconv.Itoa(int(port))
	}
	return session.Start(c.Context, session.Config{
		Version:    Version,
		Mods:       mods,
		Port:       uint16(c.Uint("port")),
		Map:        mapName,
		Args:       c.StringSlice("arg"),
		Launch:     !c.Bool("no-game"),
		HostOutput: log,
		Interface:  ui,
		Relay:      c.String("ui-relay"),
		Events:     out.host,
	})
}

// waitSession waits for the server to stop, or stops it on Ctrl-C. With
// --json it also stops the server when standard input closes, because a
// program cannot send Ctrl-C to a Windows process.
func waitSession(c *cli.Context, out *printer, running *session.Session) error {
	// Stop on Ctrl-C, or on closed input for a program.
	ctx, cancel := context.WithCancel(c.Context)
	defer cancel()
	if out.json {
		go func() {
			_, _ = io.Copy(io.Discard, os.Stdin)
			cancel()
		}()
	}

	// A stop the user asked for is not an error.
	stop := context.AfterFunc(ctx, running.Stop)
	defer stop()
	err := running.Wait()
	if ctx.Err() != nil {
		return nil
	}
	return errors.Wrap(err, "the server stopped")
}
