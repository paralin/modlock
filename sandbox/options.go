package sandbox

import (
	"io"
	"time"

	"github.com/paralin/modlock/src/wasm"
	"github.com/tetratelabs/wazero"
)

// Options configure a loaded Mod.
type Options struct {
	// Calls answers the mod's host calls, or is nil to answer each with an
	// empty reply, as a host with no game would.
	Calls func(call *wasm.Call) *wasm.Reply
	// Output receives the mod's standard output and error, or is nil to drop
	// them.
	Output io.Writer
	// Cache keeps compiled modules for later loads, or is nil for none.
	Cache wazero.CompilationCache
	// Interpret runs the module in wazero's interpreter, which starts at once,
	// rather than compiling it, which can take a minute for a large module.
	// It suits a mod loaded to run a few calls, such as a check.
	Interpret bool
	// Clock is the time the mod reads from both WASI clocks, or nil for the
	// system clocks. A check that compares two runs gives each the same
	// clock, so the time a mod reads cannot tell the runs apart.
	Clock func() time.Time
	// Random is the source of the mod's WASI random bytes, or nil for the
	// system's.
	Random io.Reader
}
