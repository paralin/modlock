package project

import (
	"context"
	"fmt"
	"io"
	"os"
	"path/filepath"

	"github.com/paralin/modlock/runtimes"
	"github.com/paralin/modlock/sandbox"
	"github.com/pkg/errors"
	"github.com/tetratelabs/wazero"
)

// twinFrames is how many frames, five seconds of the frame clock, the twin
// check runs after a mod starts.
const twinFrames = 300

// defaultMap is the world a mod that names no map starts in.
const defaultMap = "dl_midtown"

// checkTwin runs the mod's scriptc module and its QuickJS bundle in lockstep
// in the sandbox, through Start, its player, its world and twinFrames frames,
// prints how many events both builds handled the same, and returns why the
// module cannot ship: the first event on which it behaves differently from the
// bundle, or why the check could not run. The bundle is the reference, since
// QuickJS runs JavaScript as written.
func (p *Project) checkTwin(ctx context.Context, output io.Writer, module, bundle []byte) error {
	// Build the QuickJS runtime and keep compiled modules where play does, so
	// an unchanged runtime compiles once.
	interpreter, err := runtimes.QuickJS(ctx)
	if err != nil {
		return errors.Wrap(err, "the check against QuickJS cannot run")
	}
	reference, err := os.ReadFile(interpreter)
	if err != nil {
		return errors.Wrap(err, "the check against QuickJS cannot run")
	}
	var cache wazero.CompilationCache
	if dir, err := os.UserCacheDir(); err == nil {
		if cache, err = wazero.NewCompilationCacheWithDir(filepath.Join(dir, "modlock", "compiled")); err == nil {
			defer cache.Close(ctx)
		}
	}

	// Play both builds the same session.
	twin, err := sandbox.NewTwin(ctx, reference, module, nil, cache)
	if err != nil {
		return errors.Wrap(err, "the check against QuickJS cannot run")
	}
	defer twin.Close(ctx)
	world := p.Manifest.GetMap()
	if world == "" {
		world = defaultMap
	}
	if err := twin.Play(ctx, bundle, world, twinFrames); err != nil {
		var divergence *sandbox.Divergence
		if errors.As(err, &divergence) {
			return errors.Errorf("its WebAssembly build differs from QuickJS at event %d (%s): QuickJS %s, WebAssembly %s",
				divergence.Event, divergence.Method, divergence.Reference, divergence.Candidate)
		}
		return errors.Wrap(err, "the check against QuickJS cannot run")
	}
	_, _ = fmt.Fprintf(output, "The WebAssembly build behaves as the QuickJS bundle does over %d events.\n", twin.Events())
	return nil
}
