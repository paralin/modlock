// Package check decides whether a built mod may run, without a game. It is
// the check a publishing service runs before it offers a release to players:
// a WebAssembly mod must keep to the host's imports and start in the host's
// sandbox limits, and an interpreted mod must name a runtime Modlock ships.
package check

import (
	"context"
	"io/fs"
	"regexp"

	"github.com/paralin/modlock/proto/modlock/wasm"
	"github.com/paralin/modlock/sandbox"
	"github.com/pkg/errors"
)

// ManifestFile is the manifest's file name in a built mod.
const ManifestFile = sandbox.ManifestFile

// slugPattern matches a valid slug.
var slugPattern = regexp.MustCompile(`^[a-z0-9]+(-[a-z0-9]+)*$`)

// Slug reports whether slug is a valid mod slug.
func Slug(slug string) error {
	if !slugPattern.MatchString(slug) {
		return errors.Errorf("the slug %q must be lowercase letters, digits and single hyphens", slug)
	}
	return nil
}

// Mod checks the built mod in dist and returns its manifest.
func Mod(ctx context.Context, dist fs.FS) (*wasm.Manifest, error) {
	// Read the build and check its manifest.
	build, err := sandbox.ReadBuild(dist)
	if err != nil {
		return nil, err
	}
	manifest := build.Manifest
	if err := Slug(manifest.GetSlug()); err != nil {
		return nil, errors.Wrap(err, "mod.json")
	}
	if manifest.GetVersion() == "" {
		return nil, errors.New("mod.json names no version")
	}

	// Run a module; an interpreted mod's runtime is Modlock's own.
	switch {
	case manifest.GetRuntime() == wasm.Manifest_RUNTIME_WASM:
		return manifest, Module(ctx, build.Entry)
	case sandbox.Interpreter(manifest.GetRuntime()) != "":
		return manifest, nil
	default:
		return nil, errors.Errorf("mod.json names the runtime %s, which this Modlock does not run", manifest.GetRuntime())
	}
}

// Module checks that module keeps to the host's boundary and starts within
// the host's memory and time limits with every host request answered empty.
func Module(ctx context.Context, module []byte) error {
	mod, err := sandbox.Load(ctx, module, sandbox.Options{})
	if err != nil {
		return err
	}
	defer mod.Close(ctx)
	return mod.Deliver(ctx, "Start", &wasm.StartEvent{CheckOnly: true}, nil)
}
