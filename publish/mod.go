package publish

import (
	"io/fs"
	"os"
	"path"
	"path/filepath"

	"github.com/paralin/modlock/check"
	"github.com/paralin/modlock/src/wasm"
	"github.com/pkg/errors"
)

// Mod is a checked built mod.
type Mod struct {
	// Dir is the built mod's directory.
	Dir string
	// Manifest is its mod.json.
	Manifest *wasm.Manifest
	// Notes say what the release changed, for providers that show them.
	Notes string
	// Prerelease publishes to Hyperline for the mod's testers only, in its
	// Prerelease games, and leaves its games on their release.
	Prerelease bool
}

// Files lists the files a release holds: the manifest, the entry, and the map
// files, client content and data the mod ships, if any. Other files in the
// directory, such as a session's log, stay out.
func (m *Mod) Files() ([]string, error) {
	// Every release holds the manifest and its entry.
	files := []string{check.ManifestFile, m.Manifest.GetEntry()}

	// Add the map files the mod ships.
	if name := m.Manifest.GetMap(); name != "" {
		for _, extension := range []string{".vpk", ".bsp"} {
			shipped := path.Join("maps", name+extension)
			if _, err := os.Stat(filepath.Join(m.Dir, filepath.FromSlash(shipped))); err == nil {
				files = append(files, shipped)
			}
		}
	}

	// Add the client content and the data.
	for _, dir := range append(check.AddonDirs(), check.DataDir) {
		err := fs.WalkDir(os.DirFS(m.Dir), dir, func(name string, entry fs.DirEntry, err error) error {
			switch {
			case name == dir && errors.Is(err, fs.ErrNotExist):
				return nil
			case err != nil:
				return err
			case entry.Type().IsRegular():
				files = append(files, name)
			}
			return nil
		})
		if err != nil {
			return nil, errors.Wrap(err, "list the shipped files")
		}
	}
	return files, nil
}
