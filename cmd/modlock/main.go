// Command modlock creates, builds and runs Deadlock mods.
package main

import (
	"context"
	"fmt"
	"os"
	"os/signal"

	"github.com/aperturerobotics/cli"
	"github.com/paralin/modlock/internal/selfupdate"
)

// Version is the release this command line belongs to; the release build
// sets it, and it selects the matching modlock-host release.
var Version = "dev"

// main runs the command line until its command finishes or Ctrl-C stops it.
func main() {
	// Stop the session cleanly on Ctrl-C.
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt)
	defer stop()

	// Report as text, or as events once the flags ask for them.
	out := &printer{current: Version}
	selfupdate.Clean()
	app := &cli.App{
		Name:    "modlock",
		Usage:   "create, build and run Deadlock mods",
		Version: Version,
		Flags: []cli.Flag{
			&cli.BoolFlag{
				Name:  "json",
				Usage: "print progress as one JSON event per line; a server stops when standard input closes",
			},
		},
		Before: func(c *cli.Context) error {
			// Choose the output, and look for a newer release meanwhile.
			out.json = c.Bool("json")
			out.color, out.errColor = !out.json && colorful(os.Stdout), colorful(os.Stderr)
			checkForUpdate(c.Context, out, c.Args().First())
			return nil
		},
		After: func(*cli.Context) error {
			out.notice()
			return nil
		},
		Commands: []*cli.Command{
			newCommand(out),
			buildCommand(out),
			devCommand(out),
			hostCommand(out),
			joinCommand(out),
			lfsCommand(out),
			playCommand(out),
			publishCommand(out),
			loginCommand(out),
			logoutCommand(out),
			panoramaCommand(out),
			vpkCommand(out),
			kv3Command(),
			toolsCommand(out),
			updateCommand(out),
			configCommand(out),
			daemonCommand(out),
		},
	}

	// Run the command.
	if err := app.RunContext(ctx, flagsFirst(app.Commands, os.Args)); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
