package main

import (
	"context"
	"slices"
	"strings"

	"github.com/aperturerobotics/cli"
)

// flagsFirst moves a command's flags ahead of its arguments, so
// "modlock new my-mod --language go" reads as "modlock new --language go my-mod",
// and a subcommand's flags move behind the subcommand's name.
// The command line stops reading flags at the first argument and would take
// every later flag as another argument. A "--" keeps what follows as
// arguments.
func flagsFirst(commands []*cli.Command, args []string) []string {
	// Find the command whose flags to move.
	command, at := commandAt(commands, args)
	if command == nil {
		return args
	}

	// Sort the command's words into flags, with their values, and arguments.
	var flags, rest []string
	words := args[at+1:]
	for i := 0; i < len(words); i++ {
		word := words[i]
		if word == "--" {
			rest = append(rest, words[i:]...)
			break
		}
		if len(word) < 2 || word[0] != '-' {
			rest = append(rest, word)
			continue
		}
		flags = append(flags, word)
		name := strings.TrimLeft(word, "-")
		if !strings.Contains(name, "=") && takesValue(command, name) && i+1 < len(words) {
			i++
			flags = append(flags, words[i])
		}
	}
	return append(append(args[:at+1:at+1], flags...), rest...)
}

// commandAt returns the command args run and the index of its name, descending
// to a subcommand such as "panorama compile". It returns nil when args name no
// command.
func commandAt(commands []*cli.Command, args []string) (*cli.Command, int) {
	// Find the command after the program's own flags, which take no value.
	at := 1
	for at < len(args) && strings.HasPrefix(args[at], "-") {
		at++
	}
	if at == len(args) {
		return nil, 0
	}
	command := named(commands, args[at])
	if command == nil {
		return nil, 0
	}
	for at+1 < len(args) {
		sub := named(command.Subcommands, args[at+1])
		if sub == nil {
			break
		}
		command, at = sub, at+1
	}
	return command, at
}

// startArgsKey keys the mods' start arguments in a command's context.
type startArgsKey struct{}

// cutStartArgs removes the words after "--" from a command that runs mods,
// such as "modlock dev -- probe 4", and returns the context that carries them
// to startArgs. Other commands keep "--" and what follows as arguments.
func cutStartArgs(ctx context.Context, commands []*cli.Command, args []string) (context.Context, []string) {
	// Only the commands that run mods take start arguments.
	command, at := commandAt(commands, args)
	if command == nil || !command.HasName("dev") && !command.HasName("play") {
		return ctx, args
	}

	// Cut at the first "--" after the command's name.
	dash := slices.Index(args[at+1:], "--")
	if dash < 0 {
		return ctx, args
	}
	dash += at + 1
	return context.WithValue(ctx, startArgsKey{}, args[dash+1:]), args[:dash]
}

// startArgs returns the arguments cutStartArgs took for the mods' onStart
// handlers.
func startArgs(ctx context.Context) []string {
	args, _ := ctx.Value(startArgsKey{}).([]string)
	return args
}

// named returns the command called name, or nil.
func named(commands []*cli.Command, name string) *cli.Command {
	for _, c := range commands {
		if c.HasName(name) {
			return c
		}
	}
	return nil
}

// takesValue reports whether the command's flag of this name reads the next
// word as its value; a boolean flag, or an unknown one, does not.
func takesValue(command *cli.Command, name string) bool {
	for _, flag := range command.Flags {
		for _, n := range flag.Names() {
			if n == name {
				_, boolean := flag.(*cli.BoolFlag)
				return !boolean
			}
		}
	}
	return false
}
