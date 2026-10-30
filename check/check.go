// Package check decides whether a built mod may run, without a game. It is
// the check a publishing service runs before it offers a release to players:
// a WebAssembly mod must keep to the host's imports and start in the host's
// sandbox limits, an interpreted mod must name a runtime Modlock ships, the
// settings and metrics a mod declares must be well formed, and its client
// content must stay in its own addon.
package check

import (
	"context"
	"io/fs"
	"regexp"

	"github.com/paralin/modlock/metrics"
	"github.com/paralin/modlock/sandbox"
	"github.com/paralin/modlock/settings"
	"github.com/paralin/modlock/src/wasm"
	"github.com/paralin/modlock/src/wasm/manifest"
	"github.com/pkg/errors"
)

// ManifestFile is the manifest's file name in a built mod.
const ManifestFile = sandbox.ManifestFile

// DataDir is the built mod's directory of the files the mod reads.
const DataDir = sandbox.DataDir

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
func Mod(ctx context.Context, dist fs.FS) (*manifest.Manifest, error) {
	// Read the build and check its manifest.
	build, err := sandbox.ReadBuild(dist)
	if err != nil {
		return nil, err
	}
	m := build.Manifest
	if err := Slug(m.GetSlug()); err != nil {
		return nil, errors.Wrap(err, "mod.json")
	}
	if err := Version(m.GetVersion()); err != nil {
		return nil, err
	}
	if err := ContentAddon(m); err != nil {
		return nil, err
	}
	if err := Content(dist, Addon(m)); err != nil {
		return nil, err
	}
	if err := settings.Validate(m); err != nil {
		return nil, err
	}
	if err := metrics.Validate(m); err != nil {
		return nil, err
	}

	// Run a module; an interpreted mod's runtime is Modlock's own.
	switch {
	case m.GetRuntime() == manifest.Manifest_RUNTIME_WASM:
		return m, Module(ctx, build.Entry)
	case sandbox.Interpreter(m.GetRuntime()) != "":
		return m, nil
	default:
		return nil, errors.Errorf("mod.json names the runtime %s, which this Modlock does not run", m.GetRuntime())
	}
}

// Module checks that module keeps to the host's boundary and starts within
// the host's memory and time limits with every host request answered empty.
// It interprets the module, since compiling a large one takes far longer than
// the few calls the check runs.
func Module(ctx context.Context, module []byte) error {
	mod, err := sandbox.Load(ctx, module, sandbox.Options{Interpret: true})
	if err != nil {
		return err
	}
	defer mod.Close(ctx)
	return mod.Deliver(ctx, "Start", &wasm.StartEvent{CheckOnly: true}, nil)
}
