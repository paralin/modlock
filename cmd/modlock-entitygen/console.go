package main

import (
	"os"
	"path/filepath"

	"github.com/paralin/modlock/proto/modlock/dump"
	"github.com/pkg/errors"
)

// consoleTypes maps each console variable type a library can set to the Go
// type of its value. TypeScript and Luau take a bool as a boolean, a string
// as a string and every other type as a number. A variable of another type,
// such as a vector or a color, has no typed setter.
var consoleTypes = map[dump.ConsoleVariableType]string{
	dump.ConsoleVariableType_CONSOLE_VARIABLE_TYPE_BOOL:    "bool",
	dump.ConsoleVariableType_CONSOLE_VARIABLE_TYPE_INT16:   "int16",
	dump.ConsoleVariableType_CONSOLE_VARIABLE_TYPE_UINT16:  "uint16",
	dump.ConsoleVariableType_CONSOLE_VARIABLE_TYPE_INT32:   "int32",
	dump.ConsoleVariableType_CONSOLE_VARIABLE_TYPE_UINT32:  "uint32",
	dump.ConsoleVariableType_CONSOLE_VARIABLE_TYPE_INT64:   "int64",
	dump.ConsoleVariableType_CONSOLE_VARIABLE_TYPE_UINT64:  "uint64",
	dump.ConsoleVariableType_CONSOLE_VARIABLE_TYPE_FLOAT32: "float32",
	dump.ConsoleVariableType_CONSOLE_VARIABLE_TYPE_FLOAT64: "float64",
	dump.ConsoleVariableType_CONSOLE_VARIABLE_TYPE_STRING:  "string",
}

// consoleEntry is one console variable or command a library names.
type consoleEntry struct {
	// name is the variable or command, such as sv_cheats.
	name string
	// help is its help text, or empty.
	help string
	// typ is a variable's Go value type, or empty for a command.
	typ string
}

// console is what a game dump's console.json lists: the variables and
// commands, each sorted by name.
type console struct {
	variables []consoleEntry
	commands  []consoleEntry
}

// readConsole reads console.json from a game dump directory, keeping each
// variable of a type a library can set and each command whose name every
// library can spell.
func readConsole(dir string) (*console, error) {
	data, err := os.ReadFile(filepath.Join(dir, "console.json"))
	if err != nil {
		return nil, err
	}
	var dumped dump.Console
	if err := dumped.UnmarshalJSON(data); err != nil {
		return nil, errors.Wrap(err, "decode console.json")
	}
	c := &console{}
	for _, v := range dumped.GetVariables() {
		if typ, ok := consoleTypes[v.GetType()]; ok && identifier.MatchString(v.GetName()) {
			c.variables = append(c.variables, consoleEntry{name: v.GetName(), help: v.GetHelp(), typ: typ})
		}
	}
	for _, command := range dumped.GetCommands() {
		if identifier.MatchString(command.GetName()) {
			c.commands = append(c.commands, consoleEntry{name: command.GetName(), help: command.GetHelp()})
		}
	}
	return c, nil
}

// scriptType returns the TypeScript and Luau type of a variable's value.
func (e consoleEntry) scriptType() string {
	switch e.typ {
	case "bool":
		return "boolean"
	case "string":
		return "string"
	}
	return "number"
}
