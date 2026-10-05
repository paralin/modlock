package sandbox

import (
	"io/fs"

	"github.com/paralin/modlock/proto/modlock/wasm"
	"github.com/pkg/errors"
)

// ManifestFile is the manifest's file name in a built mod.
const ManifestFile = "mod.json"

// Build is a built mod as the host reads it.
type Build struct {
	// Manifest is the mod's mod.json.
	Manifest *wasm.Manifest
	// Entry is the file the manifest names: the module of a WebAssembly mod,
	// or the source an interpreter runs.
	Entry []byte
}

// ReadBuild reads the built mod in dist through its manifest.
func ReadBuild(dist fs.FS) (*Build, error) {
	// Read the manifest.
	data, err := fs.ReadFile(dist, ManifestFile)
	if err != nil {
		return nil, errors.Wrap(err, "read mod.json")
	}
	manifest := &wasm.Manifest{}
	if err := manifest.UnmarshalJSON(data); err != nil {
		return nil, errors.Wrap(err, "parse mod.json")
	}

	// Read the entry it names.
	entry := manifest.GetEntry()
	if !fs.ValidPath(entry) || entry == "." {
		return nil, errors.Errorf("mod.json entry %q is not a file in the mod", entry)
	}
	source, err := fs.ReadFile(dist, entry)
	if err != nil {
		return nil, errors.Wrap(err, "read the entry")
	}
	return &Build{Manifest: manifest, Entry: source}, nil
}

// Interpreter names the module in Modlock's host package that runs an
// interpreted runtime, or is empty for WebAssembly and unknown runtimes.
func Interpreter(runtime wasm.Manifest_Runtime) string {
	switch runtime {
	case wasm.Manifest_RUNTIME_QUICKJS:
		return "quickjs.wasm"
	case wasm.Manifest_RUNTIME_LUAU:
		return "luau.wasm"
	case wasm.Manifest_RUNTIME_PYTHON:
		return "python.wasm"
	default:
		return ""
	}
}
