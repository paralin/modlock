// Package project reads, creates and builds mod source projects. A project is
// a directory with mod.json, whose manifest names the mod and its language;
// building it writes the built mod, a directory modlock-host loads, to the
// project's build directory.
package project

import (
	"bytes"
	stdjson "encoding/json"
	"os"
	"path/filepath"

	"github.com/aperturerobotics/protobuf-go-lite/json"
	"github.com/paralin/modlock/check"
	"github.com/paralin/modlock/proto/modlock/wasm"
	"github.com/pkg/errors"
)

// ManifestFile is the manifest's file name in a project and a built mod.
const ManifestFile = check.ManifestFile

// BuildDir is the built mod's directory within a project.
const BuildDir = "build"

// Project is a mod's source directory.
type Project struct {
	// Dir is the project directory.
	Dir string
	// Manifest is the project's mod.json.
	Manifest *wasm.Manifest
}

// Open reads the project in dir.
func Open(dir string) (*Project, error) {
	// Read the manifest.
	dir, err := filepath.Abs(dir)
	if err != nil {
		return nil, errors.Wrap(err, "open the project")
	}
	manifest, err := ReadManifest(dir)
	if err != nil {
		return nil, err
	}

	// Check what the build needs.
	if err := check.Slug(manifest.GetSlug()); err != nil {
		return nil, errors.Wrap(err, "mod.json")
	}
	if manifest.GetLanguage() == wasm.Manifest_LANGUAGE_UNKNOWN {
		return nil, errors.New("mod.json names no language")
	}
	return &Project{Dir: dir, Manifest: manifest}, nil
}

// ReadManifest reads the mod.json in dir, a project or a built mod.
func ReadManifest(dir string) (*wasm.Manifest, error) {
	data, err := os.ReadFile(filepath.Join(dir, ManifestFile))
	if err != nil {
		return nil, errors.Wrap(err, "read mod.json")
	}
	manifest := &wasm.Manifest{}
	if err := manifest.UnmarshalJSON(data); err != nil {
		return nil, errors.Wrap(err, "parse mod.json")
	}
	return manifest, nil
}

// Output returns the built mod's directory.
func (p *Project) Output() string {
	return filepath.Join(p.Dir, BuildDir)
}

// WriteManifest writes manifest to path as indented JSON, one field per line,
// with enums by name. It replaces path whole.
func WriteManifest(path string, manifest *wasm.Manifest) error {
	data, err := json.MarshalerConfig{}.Marshal(manifest)
	if err != nil {
		return errors.Wrap(err, "encode mod.json")
	}
	var buf bytes.Buffer
	if err := stdjson.Indent(&buf, data, "", "  "); err != nil {
		return errors.Wrap(err, "encode mod.json")
	}
	buf.WriteString("\n")
	if err := os.WriteFile(path+".tmp", buf.Bytes(), 0o644); err != nil {
		return errors.Wrap(err, "write mod.json")
	}
	return errors.Wrap(os.Rename(path+".tmp", path), "write mod.json")
}
