package sandbox

import (
	"io"

	"github.com/paralin/modlock/proto/modlock/wasm"
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
}
