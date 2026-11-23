package console

import (
	"strings"

	"github.com/paralin/modlock/mod"
)

// Command is one console command.
type Command struct {
	// Name is the command's name, such as changelevel.
	Name string
}

// Run runs the command with args, each one word of the line.
func (c Command) Run(args ...string) error {
	return mod.ServerCommand(strings.Join(append([]string{c.Name}, args...), " "))
}
