package project

import (
	"bytes"
	"context"
	"io"
	"io/fs"
	"os"
	"os/exec"
	"path/filepath"

	"github.com/paralin/modlock/check"
	"github.com/paralin/modlock/internal/filelock"
	"github.com/paralin/modlock/metrics"
	"github.com/paralin/modlock/settings"
	"github.com/paralin/modlock/src/wasm/manifest"
	"github.com/pkg/errors"
)

// Entry is the built WebAssembly module's file name in a built mod.
const Entry = "mod.wasm"

// MapsDir is the directory of a project, and of its built mod, that holds the
// map the mod plays on when the mod ships its own: maps/<map>.vpk, and
// maps/<map>.bsp for the Quake collision its movement model walks on.
const MapsDir = "maps"

// Mode chooses what a script mod's build favors.
type Mode int

const (
	// Release is the build publishing makes. A script mod whose mod.json
	// asks for RUNTIME_WASM compiles to WebAssembly with scriptc, which runs
	// it faster, and bundles for QuickJS when scriptc cannot compile it;
	// every other script mod bundles for QuickJS. scriptc takes about a
	// minute and gigabytes of memory, so a mod asks for it only when its
	// frames run too long on QuickJS.
	Release Mode = iota
	// Dev bundles a script mod for QuickJS, which builds in a moment.
	Dev
)

// buildLock is the file in Output that a build holds exclusively, so two
// builds of one project, such as a watch's and an explicit modlock build,
// take turns instead of erasing each other's staged files.
const buildLock = ".build.lock"

// Build checks the project and writes the built mod to Output. Check and
// compiler errors go to output and fail the build with a *CheckError that
// locates them; a failed build leaves the previous built mod in place.
// Builds of one project run one at a time; a second waits for the first.
func (p *Project) Build(ctx context.Context, output io.Writer, mode Mode) error {
	// Wait for any other build of this project to finish.
	if err := os.MkdirAll(p.Output(), 0o755); err != nil {
		return errors.Wrap(err, "create the build directory")
	}
	held, err := filelock.Lock(filepath.Join(p.Output(), buildLock), true, true)
	if err != nil {
		return errors.Wrap(err, "wait for the other build")
	}
	defer held.Close()

	// Check the declarations, and ship the map and client content, before
	// compiling.
	if err := settings.Validate(p.Manifest); err != nil {
		return err
	}
	if err := metrics.Validate(p.Manifest); err != nil {
		return err
	}
	if err := p.shipMap(); err != nil {
		return err
	}
	if err := p.shipData(); err != nil {
		return err
	}
	if err := p.buildContent(); err != nil {
		return err
	}

	// Compile the mod for its language.
	switch p.Manifest.GetLanguage() {
	case manifest.Manifest_LANGUAGE_GO:
		return p.buildGo(ctx, output)
	case manifest.Manifest_LANGUAGE_TYPESCRIPT, manifest.Manifest_LANGUAGE_JAVASCRIPT:
		return p.buildScript(ctx, output, mode)
	case manifest.Manifest_LANGUAGE_LUAU:
		return p.buildLuau(ctx, output)
	case manifest.Manifest_LANGUAGE_PYTHON:
		return p.buildPython(ctx, output)
	default:
		return errors.Errorf("modlock cannot build %s mods yet", p.Manifest.GetLanguage())
	}
}

// buildGo vets the Go project and compiles it to a WebAssembly module.
func (p *Project) buildGo(ctx context.Context, output io.Writer) error {
	// Vet and compile for WASI, in the project directory, locating the
	// problems a failed step reports.
	run := func(source string, arguments ...string) error {
		// Run the step, keeping a copy of its output.
		var printed bytes.Buffer
		cmd := exec.CommandContext(ctx, "go", arguments...)
		cmd.Dir = p.Dir
		cmd.Env = append(os.Environ(), "GOOS=wasip1", "GOARCH=wasm")
		cmd.Stdout = io.MultiWriter(output, &printed)
		cmd.Stderr = cmd.Stdout

		// Locate the problems a failed step reports.
		if err := cmd.Run(); err != nil {
			return &CheckError{
				Err:         errors.Wrap(err, source),
				Diagnostics: goDiagnostics(p.Dir, source, printed.Bytes()),
			}
		}
		return nil
	}

	// Vet, then compile to a staged module.
	if err := run("go vet", "vet", "./..."); err != nil {
		return err
	}
	staged := filepath.Join(p.Output(), Entry+".tmp")
	if err := run("go build", "build", "-buildmode=c-shared", "-o", staged, "."); err != nil {
		_ = os.Remove(staged)
		return err
	}

	// Replace the module, then the manifest that names it.
	if err := os.Rename(staged, filepath.Join(p.Output(), Entry)); err != nil {
		return errors.Wrap(err, "write the module")
	}
	return p.writeBuiltManifest(manifest.Manifest_RUNTIME_WASM, Entry)
}

// writeBuiltManifest writes the built mod's mod.json: the project's manifest,
// naming the runtime and entry in place of the language.
func (p *Project) writeBuiltManifest(runtime manifest.Manifest_Runtime, entry string) error {
	// Copy the manifest, naming the runtime and the entry.
	built := p.Manifest.CloneVT()
	built.Language = manifest.Manifest_LANGUAGE_UNKNOWN
	built.Runtime = runtime
	built.Entry = entry
	return WriteManifest(filepath.Join(p.Output(), ManifestFile), built)
}

// shipMap copies the map files the manifest names from the project's maps
// directory into the built mod. A mod that plays a stock map has none.
func (p *Project) shipMap() error {
	for _, extension := range []string{".vpk", ".bsp"} {
		if err := p.shipMapFile(p.Manifest.GetMap() + extension); err != nil {
			return err
		}
	}
	return nil
}

// shipData copies the project's data directory, the files the mod reads
// with readFile such as data/areas.yaml, into the built mod's,
// replacing the previous one; a failed copy leaves it in place. A project
// without data ships none.
func (p *Project) shipData() error {
	// Stage a copy of the project's data.
	final := filepath.Join(p.Output(), check.DataDir)
	staged := final + ".tmp"
	if err := os.RemoveAll(staged); err != nil {
		return errors.Wrap(err, "copy the data")
	}
	defer os.RemoveAll(staged)
	source := filepath.Join(p.Dir, check.DataDir)
	_, err := os.Stat(source)
	shipped := err == nil
	if shipped {
		if err := os.CopyFS(staged, os.DirFS(source)); err != nil {
			return errors.Wrap(err, "copy the data")
		}
	} else if !errors.Is(err, fs.ErrNotExist) {
		return errors.Wrap(err, "read the data")
	}

	// Replace the previous data with it.
	if err := os.RemoveAll(final); err != nil {
		return errors.Wrap(err, "copy the data")
	}
	if !shipped {
		return nil
	}
	return errors.Wrap(os.Rename(staged, final), "copy the data")
}

// shipMapFile copies one map file, when the project has it.
func (p *Project) shipMapFile(name string) error {
	// Open the project's map file; a stock map has none.
	if p.Manifest.GetMap() == "" || !filepath.IsLocal(name) {
		return nil
	}
	source, err := os.Open(filepath.Join(p.Dir, MapsDir, name))
	if errors.Is(err, fs.ErrNotExist) {
		return nil
	}
	if err != nil {
		return errors.Wrap(err, "read the map")
	}
	defer source.Close()

	// Stage the copy so a failed one leaves the previous map in place.
	dir := filepath.Join(p.Output(), MapsDir)
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return errors.Wrap(err, "create the maps directory")
	}
	staged, err := os.CreateTemp(dir, name+".*.tmp")
	if err != nil {
		return errors.Wrap(err, "copy the map")
	}
	defer os.Remove(staged.Name())
	if _, err := io.Copy(staged, source); err != nil {
		staged.Close()
		return errors.Wrap(err, "copy the map")
	}

	// CreateTemp makes the file private; a shipped map is readable like the
	// rest of the build.
	if err := staged.Chmod(0o644); err != nil {
		staged.Close()
		return errors.Wrap(err, "copy the map")
	}
	if err := staged.Close(); err != nil {
		return errors.Wrap(err, "copy the map")
	}
	return errors.Wrap(os.Rename(staged.Name(), filepath.Join(dir, name)), "copy the map")
}
