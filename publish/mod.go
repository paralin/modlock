package publish

import (
	"os"
	"path"
	"path/filepath"

	"github.com/paralin/modlock/check"
	"github.com/paralin/modlock/proto/modlock/wasm"
)

// Mod is a checked built mod.
type Mod struct {
	// Dir is the built mod's directory.
	Dir string
	// Manifest is its mod.json.
	Manifest *wasm.Manifest
}

// Files lists the files a release holds: the manifest, the entry and the map
// files the mod ships, if any. Other files in the directory, such as a
// session's log, stay out.
func (m *Mod) Files() []string {
	files := []string{check.ManifestFile, m.Manifest.GetEntry()}
	name := m.Manifest.GetMap()
	if name == "" {
		return files
	}
	for _, extension := range []string{".vpk", ".bsp"} {
		shipped := path.Join("maps", name+extension)
		if _, err := os.Stat(filepath.Join(m.Dir, filepath.FromSlash(shipped))); err == nil {
			files = append(files, shipped)
		}
	}
	return files
}
