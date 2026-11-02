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

// TracesDir is a project's directory of traces, sessions a game server
// recorded with MODLOCK_TRACE_DIR set. A release build replays each one to
// its WebAssembly build beside the QuickJS bundle. Publish ships none of
// them.
const TracesDir = "traces"

// checkTwin runs the mod's scriptc module and its QuickJS bundle in lockstep
// in the sandbox, through Start, its player, its world and twinFrames frames,
// then through each trace in TracesDir, prints how many events both builds
// handled the same, and returns why the module cannot ship: the first event
// on which it behaves differently from the bundle, or why the check could not
// run. The bundle is the reference, since QuickJS runs JavaScript as written.
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

	// pair plays both builds one session, from a fresh load, and counts its
	// events; session names a traced one in a divergence.
	pair := func(session string, play func(*sandbox.Twin) error) (int, error) {
		twin, err := sandbox.NewTwin(ctx, reference, module, nil, cache)
		if err != nil {
			return 0, errors.Wrap(err, "the check against QuickJS cannot run")
		}
		defer twin.Close(ctx)
		err = play(twin)
		var divergence *sandbox.Divergence
		switch {
		case errors.As(err, &divergence):
			return 0, errors.Errorf("its WebAssembly build differs from QuickJS at event %d%s (%s): QuickJS %s, WebAssembly %s",
				divergence.Event, session, divergence.Method, divergence.Reference, divergence.Candidate)
		case err != nil:
			return 0, errors.Wrap(err, "the check against QuickJS cannot run")
		}
		return twin.Events(), nil
	}

	// Play a sandbox session.
	world := p.Manifest.GetMap()
	if world == "" {
		world = defaultMap
	}
	events, err := pair("", func(twin *sandbox.Twin) error { return twin.Play(ctx, bundle, world, twinFrames) })
	if err != nil {
		return err
	}

	// Replay each traced session.
	traces, err := filepath.Glob(filepath.Join(p.Dir, TracesDir, "*.trace"))
	if err != nil {
		return errors.Wrap(err, "the check against QuickJS cannot run")
	}
	for _, path := range traces {
		session := " of " + TracesDir + "/" + filepath.Base(path)
		replayed, err := pair(session, func(twin *sandbox.Twin) error {
			traced, err := os.Open(path)
			if err != nil {
				return err
			}
			defer traced.Close()
			return twin.Replay(ctx, traced, bundle)
		})
		if err != nil {
			return err
		}
		events += replayed
	}
	_, _ = fmt.Fprintf(output, "The WebAssembly build behaves as the QuickJS bundle does over %d events in %d sessions.\n", events, 1+len(traces))
	return nil
}
