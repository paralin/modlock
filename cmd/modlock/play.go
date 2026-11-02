package main

import (
	"bufio"
	"context"
	"os"
	"path/filepath"
	"slices"
	"strconv"
	"strings"

	"github.com/aperturerobotics/cli"
	modcli "github.com/paralin/modlock/cli"
	"github.com/paralin/modlock/js"
	"github.com/paralin/modlock/project"
	"github.com/paralin/modlock/sandbox"
	"github.com/paralin/modlock/session"
	"github.com/paralin/modlock/src/host_app/control"
	"github.com/pkg/errors"
)

// sessionFlags configure the server and client of a session.
var sessionFlags = append([]cli.Flag{
	&cli.UintFlag{Name: "port", Value: session.DefaultPort, Usage: "the server's UDP port"},
	&cli.BoolFlag{Name: "no-game", Usage: "run only the server; join it yourself"},
	&cli.BoolFlag{
		Name:  "sandbox",
		Usage: "run the mods without the game, with a stand-in player, even where the game runs",
	},
	&cli.StringSliceFlag{Name: "arg", Usage: "pass an argument to the mods' start handlers; repeat for several"},
}, interfaceFlags...)

// interfaceFlags configure how the game shows the mods' interfaces.
var interfaceFlags = []cli.Flag{
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

// playCommand runs built mods locally, optionally watching a project or
// serving the session to friends.
func playCommand(out *printer) *cli.Command {
	return &cli.Command{
		Name:        "play",
		Usage:       "play mods locally or let friends join this PC's server",
		Description: "Play built mods or projects, building projects with missing output. Without the game, use the sandbox. --watch builds and watches one project.\n\nExamples:\n  modlock play\n  modlock play --lobby\n  modlock play --lobby --watch\n  modlock play --host",
		ArgsUsage:   "[MOD...]",
		Flags: append(slices.Clone(sessionFlags),
			releaseFlag,
			&cli.BoolFlag{Name: "watch", Usage: "build one mod project and reload it on each save"},
			&cli.BoolFlag{Name: "host", Usage: "let friends join this PC's server with a join code"},
			&cli.BoolFlag{Name: "lobby", Usage: "let friends join this PC's server through a Hyperline lobby"},
			&cli.UintFlag{Name: "preview-port", Value: previewPort, Usage: "the watched project's browser preview port; 0 turns it off"},
		),
		Action: func(c *cli.Context) error {
			// Watching uses the project builder and keeps hosting independent.
			if c.Bool("watch") {
				if c.Args().Len() > 1 {
					return errors.New("--watch takes one mod project; pass one directory")
				}
				return runDev(c, out, "play")
			}

			// Take each mod as a built mod or a project's build, building a
			// project that has none yet, or every project with --release.
			paths := c.Args().Slice()
			if len(paths) == 0 {
				paths = []string{"."}
			}
			out.header("play", strings.Join(paths, " "))
			mods := make([]string, 0, len(paths))
			for _, path := range paths {
				dir, err := filepath.Abs(path)
				if err != nil {
					return err
				}
				opened, err := project.Open(dir)
				if err == nil {
					dir = opened.Output()
				}
				_, err = os.Stat(filepath.Join(dir, project.ManifestFile))
				if err != nil && opened == nil {
					return errors.Errorf("%s is neither a mod project nor a built mod", path)
				}
				// --release rebuilds a project, so an earlier dev build
				// never stands in for the release.
				if opened != nil && (err != nil || c.Bool("release")) {
					if err := out.build(c.Context, opened, buildMode(c)); err != nil {
						return err
					}
				}
				mods = append(mods, dir)
			}

			// Capture guest interfaces before starting the server.
			feed := observeHosting(c, out)

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
			if feed != nil {
				go hostSession(c, out, running, feed, mods)
			}
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

	// Start a server unless the game cannot run here, naming the source
	// lines of the mods' slow frames.
	events := locateFrames(mods, out.host)
	reason := "--sandbox asked for it"
	if !c.Bool("sandbox") {
		running, err := startServer(c, out, events, mods, mapName, logPath)
		if err == nil {
			return running, nil
		}
		if !errors.Is(err, session.ErrNoGame) {
			return nil, err
		}
		reason = err.Error()
	}

	// Run the mods in the sandbox, with the interpreters from the host's
	// package and the modules compiled in earlier sessions.
	out.sandbox(reason)
	compiled := ""
	if cache, err := os.UserCacheDir(); err == nil {
		compiled = filepath.Join(cache, "modlock", "compiled")
	}
	return sandbox.Start(c.Context, sandbox.Config{
		Mods:     mods,
		Map:      mapName,
		Args:     c.StringSlice("arg"),
		Settings: configPath("settings.json"),
		Files:    configPath("files"),
		Compiled: compiled,
		Interpreters: func(ctx context.Context) (string, error) {
			host, err := session.FindHost(ctx, Version)
			return filepath.Dir(host), err
		},
		Events: events,
	}), nil
}

// startServer starts a local server with mods on mapName, writing its console
// to logPath and passing the host's events to events.
func startServer(c *cli.Context, out *printer, events func(*control.HostEvent), mods []string, mapName, logPath string) (*session.Session, error) {
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
	var renderer *session.Renderer
	if port := c.Uint("ui-port"); port != 0 {
		ui = "127.0.0.1:" + strconv.Itoa(int(port))
		renderer, err = readRenderer(c.Context)
		if err != nil {
			out.mark("!", yellow, "mod interfaces show only in the browser preview: "+err.Error())
		}
	}
	running, err := session.Start(c.Context, session.Config{
		Version:    Version,
		Mods:       mods,
		Port:       uint16(c.Uint("port")),
		Map:        mapName,
		Args:       c.StringSlice("arg"),
		Settings:   configPath("settings.json"),
		Files:      configPath("files"),
		Launch:     !c.Bool("no-game"),
		HostOutput: log,
		Interface:  ui,
		Relay:      c.String("ui-relay"),
		Renderer:   renderer,
		Events:     events,
	})
	if err != nil {
		return nil, err
	}

	// Steam cannot connect a game that is already running, so name the
	// console line that joins.
	out.field("host", running.Executable)
	out.field("console", logPath)
	out.field("join", "connect 127.0.0.1:"+strconv.Itoa(int(c.Uint("port")))+" in a running game's console")
	out.mark("…", dim, "starting the server")
	return running, nil
}

// readRenderer returns the built renderer the game draws the mods'
// interfaces with.
func readRenderer(ctx context.Context) (*session.Renderer, error) {
	// Read the renderer and layout from the installed library.
	library, err := js.Library(ctx)
	if err != nil {
		return nil, err
	}
	script, err := os.ReadFile(filepath.Join(library, js.Renderer))
	if err != nil {
		return nil, err
	}
	layout, err := os.ReadFile(filepath.Join(library, js.LayoutLibrary))
	if err != nil {
		return nil, err
	}
	return &session.Renderer{Script: script, Layout: layout}, nil
}

// configPath returns name in Modlock's configuration directory, which keeps
// players' settings and the files mods write across sessions, or empty when
// this computer names no configuration directory.
func configPath(name string) string {
	config, err := os.UserConfigDir()
	if err != nil {
		return ""
	}
	return filepath.Join(config, "modlock", name)
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
	if note := deliver(ctx, box, in); note != "" {
		out.note(note)
	}
}

// deliver presses the button or sends the command in to the sandbox's mods,
// and returns a line for the creator when no mod received it, or empty.
func deliver(ctx context.Context, box *sandbox.Sandbox, in *modcli.Input) string {
	// Press the button.
	if press := in.GetPress(); press != nil {
		pressed, err := box.Press(ctx, press.GetMod(), press.GetSlot(), press.GetNode())
		if err == nil && !pressed {
			return press.GetMod() + " is not running to receive the press"
		}
		return ""
	}

	// Change the input.
	if change := in.GetChange(); change != nil {
		changed, err := box.Input(ctx, change.GetMod(), change.GetEvent())
		if err == nil && !changed {
			return change.GetMod() + " is not running to receive the change"
		}
		return ""
	}

	// Send the command.
	command := strings.TrimSpace(in.GetCommand())
	handled, err := box.Command(ctx, command)
	if err == nil && !handled && command != "" {
		return "no mod handled " + command
	}
	return ""
}
