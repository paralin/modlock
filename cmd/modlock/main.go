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
		Name:  "modlock",
		Usage: "create, build and run Deadlock mods",
		Description: "Start a mod with `modlock new`, then run it with `modlock dev`.\n" +
			"`modlock help COMMAND` describes one command.",
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
		CommandNotFound: func(c *cli.Context, name string) {
			// Offer the nearest command for a mistyped one.
			fmt.Fprintln(os.Stderr, "modlock has no "+name+" command; run modlock help for the list")
			if nearest := cli.SuggestCommand(c.App.Commands, name); nearest != "" {
				fmt.Fprintln(os.Stderr, nearest)
			}
			os.Exit(2)
		},
		Commands: append([]*cli.Command{
			newCommand(out),
			devCommand(out),
			buildCommand(out),
			playCommand(out),
			joinCommand(out),
			publishCommand(out),
			loginCommand(out),
			logoutCommand(out),
			updateCommand(out),
			editorCommand(out),
		}, tools(
			studioCommand(out),
			panoramaCommand(out),
			vpkCommand(out),
			kv3Command(),
			lfsCommand(out),
			configCommand(out),
			daemonCommand(out),
		)...),
	}

	// Tell a person who opened modlock from Explorer how to use it, since
	// its window would close before they read anything.
	if len(os.Args) == 1 && ownConsole() {
		explain()
		return
	}

	// Run the command.
	if err := app.RunContext(ctx, flagsFirst(app.Commands, os.Args)); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}

// tools files commands under the help's tools heading, below the commands a
// new mod author starts with.
func tools(commands ...*cli.Command) []*cli.Command {
	for _, command := range commands {
		command.Category = "tools"
	}
	return commands
}

// explain tells a person who opened modlock by double-clicking it that it
// runs in a terminal and how to install it there, then waits for Enter so
// the window stays open to be read.
func explain() {
	fmt.Print("modlock creates, builds and runs Deadlock mods. It is a command-line\n" +
		"program: you type its commands in a terminal, and this window only\n" +
		"opened because you double-clicked it.\n\n" +
		"To install it, open PowerShell from the Start menu and paste this line:\n\n" +
		"  irm https://raw.githubusercontent.com/paralin/modlock/master/install.ps1 | iex\n\n" +
		"Then make your first mod there with:\n\n" +
		"  modlock new\n\n" +
		"Press Enter to close this window.\n")
	_, _ = fmt.Scanln()
}
