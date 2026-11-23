package console

import (
	"fmt"

	"github.com/paralin/modlock/mod"
)

// Value lists the Go types of console variables.
type Value interface {
	bool | int8 | uint8 | int16 | uint16 | int32 | uint32 | int64 | uint64 | float32 | float64 | string
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
