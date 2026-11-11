package sandbox

import (
	"context"

	"github.com/paralin/modlock/src/host_app/control"
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
	// Files is the directory that keeps the files mods write, or empty to
	// refuse their writes.
	Files string
	// Compiled is the directory that keeps compiled modules across sessions,
	// so a later session loads the interpreters and an unchanged mod without
	// compiling them, or empty to compile each module once per session.
	Compiled string
	// Fixtures returns the directory of service fixtures for the built mod at
	// path, or empty when it has none; see FixtureDir. Nil gives no mod
	// fixtures.
	Fixtures func(path string) string
}
