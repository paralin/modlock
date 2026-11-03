// Package scriptc downloads the scriptc compiler and fits a module it
// compiled as a WASI reactor to the mod boundary, so the host runs it as it
// runs any other mod.
//
// scriptc imports its host callbacks and its panic sink from the scriptc
// module, starts with _initialize and a separate init export, and returns an
// export's bytes through two out-pointers. The fit renames the callbacks'
// module to modlock, replaces the panic import with a function that logs the
// panic through host_call and traps, makes _initialize run the init export,
// and adds modlock_event, which packs the reply's address and length.
//
// LowerJSX prepares a mod's TSX sources for scriptc, which compiles no JSX.
package scriptc

import (
	"bytes"
	"encoding/json"
	"fmt"

	"github.com/pkg/errors"
)

// The build's scriptc profile declares these symbols.
const (
	// InitExport is the profile's init symbol, which sets up the library.
	InitExport = "mod_init"
	// EventExport is the profile's event export. It takes the size of the
	// call the host holds and returns the encoded reply.
	EventExport = "mod_event"
)

// profile is the scriptc profile of a mod, with its entry left to fill in.
// It names the symbols above and the host callbacks the fit renames.
const profile = `{
  "profile_format": 1,
  "name": "mod",
  "entry": %s,
  "emission": "llvm",
  "abi": {
    "prefix": "mod_",
    "init_symbol": "mod_init",
    "sink_register_symbol": "mod_sink",
    "callback_register_symbol": "mod_callback"
  },
  "callbacks": [
    { "name": "host_call", "params": ["bytes"], "returns": "u32" },
    { "name": "host_read", "params": ["bytes"], "returns": "void" }
  ],
  "exports": [
    {
      "export": "event",
      "symbol": "mod_event",
      "params": ["u32"],
      "returns": "bytes"
    }
  ]
}
`

// Profile returns the scriptc profile that builds the module at entry, a path
// relative to the profile, into one that Fit accepts. The program declares
// host_call and host_read, and the entry exports event.
func Profile(entry string) []byte {
	quoted, _ := json.Marshal(entry)
	return fmt.Appendf(nil, profile, quoted)
}

// header opens every WebAssembly binary: the magic bytes and version 1.
var header = []byte{0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00}

// section is one section of a module.
type section struct {
	// id identifies the section's kind.
	id byte
	// payload holds the section's contents.
	payload []byte
}

// Fit returns wasm, a module scriptc built with the profile's InitExport and
// EventExport, rewritten to import only from modlock and WASI and to export
// memory, _initialize and modlock_event.
func Fit(wasm []byte) ([]byte, error) {
	// Split the module into its sections.
	if !bytes.HasPrefix(wasm, header) {
		return nil, errors.New("the module is not WebAssembly version 1")
	}
	r := &reader{data: wasm, pos: len(header)}
	var sections []section
	for !r.done() {
		id := r.byte()
		sections = append(sections, section{id: id, payload: r.bytes(int(r.u32()))})
	}
	if r.err != nil {
		return nil, errors.Wrap(r.err, "read the module")
	}

	// Rewrite each section in order; each one learns what later ones need.
	m := &module{}
	out := bytes.Clone(header)
	for _, s := range sections {
		if s.id == sectionExport && !m.hasGlobals {
			out = appendSection(out, sectionGlobal, m.globals(nil))
		}
		payload, keep, err := m.rewrite(s)
		if err != nil {
			return nil, err
		}
		if keep {
			out = appendSection(out, s.id, payload)
		}
	}
	switch {
	case !m.hasPanic:
		return nil, errors.New("the module does not import scriptc.panic")
	case !m.hasCode:
		return nil, errors.New("the module has no code")
	}
	return out, nil
}

// appendSection appends a section with its id and size.
func appendSection(out []byte, id byte, payload []byte) []byte {
	out = appendU32(append(out, id), uint32(len(payload)))
	return append(out, payload...)
}
