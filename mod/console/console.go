// Package console sets the server's console variables and runs its console
// commands, through values modlock-entitygen generates from the game's
// console:
//
//	err := console.SvCheats.Set(true)
//	err = console.Changelevel.Run("street_test")
//
// Each runs one line at the server console through mod.ServerCommand, which
// may set a development-only or cheat-protected variable. A variable of a
// type other than a scalar or a string, such as a vector, has no value here;
// set it with mod.ServerCommand.
package console

import (
	"fmt"
	"strings"

	"github.com/paralin/modlock/mod"
)

// Value lists the Go types of console variables.
type Value interface {
	bool | int16 | uint16 | int32 | uint32 | int64 | uint64 | float32 | float64 | string
}

// Variable is one console variable, whose value is a T.
type Variable[T Value] struct {
	// Name is the variable's name, such as sv_cheats.
	Name string
}

// Set sets the variable to value.
func (v Variable[T]) Set(value T) error {
	return mod.ServerCommand(v.Name + " " + format(value))
}

// format returns value as the console reads it: a bool as 1 or 0 and a
// string in quotes.
func format(value any) string {
	switch value := value.(type) {
	case bool:
		if value {
			return "1"
		}
		return "0"
	case string:
		return `"` + value + `"`
	}
	return fmt.Sprint(value)
}

// Command is one console command.
type Command struct {
	// Name is the command's name, such as changelevel.
	Name string
}

// Run runs the command with args, each one word of the line.
func (c Command) Run(args ...string) error {
	return mod.ServerCommand(strings.Join(append([]string{c.Name}, args...), " "))
}
