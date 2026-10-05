package sandbox

import (
	"context"

	"github.com/paralin/modlock/proto/modlock/control"
)

// Config describes a sandbox session.
type Config struct {
	// Mods lists the built mods to run.
	Mods []string
	// Map names the world the mods start in, or is empty for none.
	Map string
	// Args are passed to each mod's start handlers.
	Args []string
	// Interpreters returns the directory that holds the interpreted
	// runtimes' modules, such as quickjs.wasm. It is called the first time an
	// interpreted mod loads.
	Interpreters func(ctx context.Context) (string, error)
	// Events receives each event a server would report, on one goroutine.
	Events func(*control.HostEvent)
	// Settings is the file that keeps the player's settings, or empty to
	// keep them for the session only.
	Settings string
}
