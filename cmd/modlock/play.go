package main

import (
	"bufio"
	"context"
	"os"
	"path/filepath"
	"strconv"
	"strings"

	"github.com/aperturerobotics/cli"
	"github.com/paralin/modlock/project"
	modcli "github.com/paralin/modlock/proto/modlock/cli"
	"github.com/paralin/modlock/sandbox"
	"github.com/paralin/modlock/session"
	"github.com/pkg/errors"
)

// sessionFlags configure the server and client of a session.
var sessionFlags = []cli.Flag{
	&cli.UintFlag{Name: "port", Value: session.DefaultPort, Usage: "the server's UDP port"},
	&cli.BoolFlag{Name: "no-game", Usage: "run only the server; join it yourself"},
	&cli.BoolFlag{
		Name:  "sandbox",
		Usage: "run the mods without the game, with a stand-in player, even where the game runs",
	},
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
		Usage:     "run built mods in a local server and join it, or in the sandbox without the game",
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

// server is a running session: a local Deadlock server, or the sandbox.
type server interface {
	// Reload replaces the running mod with the build at path.
	Reload(ctx context.Context, path string) error
	// Wait waits for the session to end.
	Wait() error
	// Stop ends the session.
	Stop()
}

// startSession starts mods on the first map a mod names: in a local server
// that writes its console to logPath, or in the sandbox when this computer
// cannot run the game or --sandbox asks for it.
func startSession(c *cli.Context, out *printer, mods []string, logPath string) (server, error) {
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

	// Start a server unless the game cannot run here.
	reason := "--sandbox asked for it"
	if !c.Bool("sandbox") {
		running, err := startServer(c, out, mods, mapName, logPath)
		if err == nil {
			return running, nil
		}
		if !errors.Is(err, session.ErrNoGame) {
			return nil, err
		}
		reason = err.Error()
	}

	// Run the mods in the sandbox, with the interpreters from the host's
	// package.
	out.sandbox(reason)
	return sandbox.Start(c.Context, sandbox.Config{
		Mods: mods,
		Map:  mapName,
		Args: c.StringSlice("arg"),
		Interpreters: func(ctx context.Context) (string, error) {
			host, err := session.FindHost(ctx, Version)
			return filepath.Dir(host), err
		},
		Events: out.host,
	}), nil
}

// startServer starts a local server with mods on mapName, writing its console
// to logPath.
func startServer(c *cli.Context, out *printer, mods []string, mapName, logPath string) (*session.Session, error) {
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

	// Start the server, which launches the client once it is ready, serving
	// the mods' interfaces unless they are off.
	var ui string
	if port := c.Uint("ui-port"); port != 0 {
		ui = "127.0.0.1:" + strconv.Itoa(int(port))
	}
	running, err := session.Start(c.Context, session.Config{
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
	if err != nil {
		return nil, err
	}

	// Steam cannot connect a game that is already running, so name the
	// console line that joins.
	out.note("starting the server; its console is in", logPath)
	out.note("to join from a running game, enter in its console: connect 127.0.0.1:" + strconv.Itoa(int(c.Uint("port"))))
	return running, nil
}

// waitSession waits for the session to end, or ends it on Ctrl-C. In the
// sandbox each line of standard input is the player's input: a command, or
// with --json a cli.Input. With --json the session also ends when standard
// input closes, because a program cannot send Ctrl-C to a Windows process.
func waitSession(c *cli.Context, out *printer, running server) error {
	// Read the player's input, and stop on closed input for a program.
	ctx, cancel := context.WithCancel(c.Context)
	defer cancel()
	box, sandboxed := running.(*sandbox.Sandbox)
	if out.json || sandboxed {
		go func() {
			lines := bufio.NewScanner(os.Stdin)
			for lines.Scan() {
				if sandboxed {
					input(ctx, out, box, lines.Text())
				}
			}
			if out.json {
				cancel()
			}
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

// input passes one line of the player's input to the sandbox's mods and
// notes input no mod received. A person types commands; a program sends a
// cli.Input as JSON.
func input(ctx context.Context, out *printer, box *sandbox.Sandbox, line string) {
	// Read a program's input; a person's line is a command.
	in := &modcli.Input{Body: &modcli.Input_Command{Command: line}}
	if out.json {
		in = &modcli.Input{}
		if err := in.UnmarshalJSON([]byte(line)); err != nil {
			out.note("the input is not a cli.Input:", err)
			return
		}
	}

	// Press the button, or send the command.
	if press := in.GetPress(); press != nil {
		pressed, err := box.Press(ctx, press.GetMod(), press.GetSlot(), press.GetNode())
		if err == nil && !pressed {
			out.note(press.GetMod(), "is not running to receive the press")
		}
		return
	}
	command := strings.TrimSpace(in.GetCommand())
	handled, err := box.Command(ctx, command)
	if err == nil && !handled && command != "" {
		out.note("no mod handled", command)
	}
}
