package project

import (
	"archive/zip"
	"bytes"
	"io/fs"
	"os"
	"path"
	"path/filepath"
	"slices"
	"strings"

	"github.com/paralin/modlock/proto/modlock/wasm"
	"github.com/pkg/errors"
)

// SourcesEntry is the zip of a Luau or Python mod's sources in a built mod,
// which its runtime reads in place.
const SourcesEntry = "mod.zip"

// LibraryDir is where a Luau or Python project's copy of the modlock
// library lives, so the checker and editors resolve it.
const LibraryDir = ".modlock"

// sourceFiles returns the slash paths of the files in dir with one of
// extensions, sorted, leaving out the build and hidden directories such as
// the installed library.
func sourceFiles(dir string, extensions ...string) ([]string, error) {
	var found []string
	err := fs.WalkDir(os.DirFS(dir), ".", func(name string, entry fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if entry.IsDir() && name != "." && (name == BuildDir || strings.HasPrefix(entry.Name(), ".")) {
			return fs.SkipDir
		}
		if !entry.IsDir() && slices.Contains(extensions, path.Ext(name)) {
			found = append(found, name)
		}
		return nil
	})
	return found, errors.Wrap(err, "read the sources")
}

// installTree copies every file of library into the project in dir under
// LibraryDir, leaving files that already match alone.
func installTree(dir string, library fs.FS) error {
	return fs.WalkDir(library, ".", func(name string, entry fs.DirEntry, err error) error {
		if err != nil || entry.IsDir() {
			return err
		}
		content, err := fs.ReadFile(library, name)
		if err != nil {
			return err
		}
		target := filepath.Join(dir, LibraryDir, filepath.FromSlash(name))
		if existing, err := os.ReadFile(target); err == nil && bytes.Equal(existing, content) {
			return nil
		}
		if err := os.MkdirAll(filepath.Dir(target), 0o755); err != nil {
			return errors.Wrap(err, "install the modlock library")
		}
		return errors.Wrap(os.WriteFile(target, content, 0o644), "install the modlock library")
	})
}

// writeSources zips the project's sources, then each file of library under
// the name rename gives it, and writes the built mod for runtime. rename
// returns "" for a file the mod leaves out.
func (p *Project) writeSources(runtime wasm.Manifest_Runtime, sources []string, library fs.FS, rename func(string) string) error {
	// Zip the sources, then the library.
	var archive bytes.Buffer
	writer := zip.NewWriter(&archive)
	for _, name := range sources {
		data, err := os.ReadFile(filepath.Join(p.Dir, filepath.FromSlash(name)))
		if err != nil {
			return errors.Wrap(err, "read the sources")
		}
		if err := store(writer, name, data); err != nil {
			return err
		}
	}
	err := fs.WalkDir(library, ".", func(name string, entry fs.DirEntry, err error) error {
		if err != nil || entry.IsDir() || rename(name) == "" {
			return err
		}
		data, err := fs.ReadFile(library, name)
		if err != nil {
			return err
		}
		return store(writer, rename(name), data)
	})
	if err != nil {
		return err
	}
	if err := writer.Close(); err != nil {
		return errors.Wrap(err, "zip the sources")
	}

	// Replace the zip, then the manifest that names it.
	if err := os.MkdirAll(p.Output(), 0o755); err != nil {
		return errors.Wrap(err, "create the build directory")
	}
	staged := filepath.Join(p.Output(), SourcesEntry+".tmp")
	if err := os.WriteFile(staged, archive.Bytes(), 0o644); err != nil {
		return errors.Wrap(err, "write the sources")
	}
	if err := os.Rename(staged, filepath.Join(p.Output(), SourcesEntry)); err != nil {
		return errors.Wrap(err, "write the sources")
	}
	return p.writeBuiltManifest(runtime, SourcesEntry)
}

// store adds data to writer as the uncompressed file name, which the runtime
// reads in place.
func store(writer *zip.Writer, name string, data []byte) error {
	file, err := writer.CreateHeader(&zip.FileHeader{Name: name, Method: zip.Store})
	if err != nil {
		return errors.Wrap(err, "zip the sources")
	}
	_, err = file.Write(data)
	return errors.Wrap(err, "zip the sources")
}
