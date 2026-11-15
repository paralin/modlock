package project

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"slices"
	"strings"

	"github.com/paralin/modlock/binaryen"
	"github.com/paralin/modlock/js"
	"github.com/paralin/modlock/scriptc"
	"github.com/pkg/errors"
)

// scriptcEntryDir is the directory of the build's entry within the mirrored
// project.
const scriptcEntryDir = ".scriptc"

// scriptcEntry is the build's entry. It loads the library's scriptc module,
// which binds the host callbacks, before the mod's main module, so the main
// module's top level can call the host, and exports the library's event
// dispatch, since scriptc exports only functions the entry declares.
const scriptcEntry = `import { event as dispatch } from 'modlock/scriptc'
import '../%s'

export function event(size: number): Uint8Array {
  return dispatch(size)
}
`

// scriptcConfig is the TypeScript configuration of the scriptc build. It
// extends the project's, compiles only the entry and what it imports, and
// maps the library and the protobuf runtime to their TypeScript source, which
// scriptc compiles with the mod in place of their bundles and declarations.
type scriptcConfig struct {
	// Extends is the project's configuration, if it has one.
	Extends string `json:"extends,omitempty"`
	// Files is the entry alone.
	Files []string `json:"files"`
	// Include is empty, so the project's include adds no files.
	Include []string `json:"include"`
	// CompilerOptions are the options the build sets over the project's.
	CompilerOptions struct {
		// NoUncheckedIndexedAccess is off: the one program spans the
		// protobuf runtime, written without it.
		NoUncheckedIndexedAccess bool `json:"noUncheckedIndexedAccess"`
		// Paths map the library and the protobuf runtime to their source.
		Paths map[string][]string `json:"paths"`
	} `json:"compilerOptions"`
}

// writeScriptcConfig writes the scriptc build's TypeScript configuration into
// dir, mapping the library to its source in library and keeping the project's
// own path mappings, which name files of the mirrored project. A project that
// maps the library to its own source keeps its mappings for every library
// module, so the program holds one library rather than a second copy; its
// program must then include the library's scriptc.ts, which the entry imports.
func (p *Project) writeScriptcConfig(dir, library string, own map[string][]string) error {
	// Extend the project's configuration, if it has one.
	config := scriptcConfig{Files: []string{"entry.ts"}, Include: []string{}}
	if project := filepath.Join(p.Dir, "tsconfig.json"); isFile(project) {
		config.Extends = filepath.ToSlash(project)
	}

	// Map the library and the protobuf runtime to their source. The runtime
	// exports each well-known type as google/protobuf/<name>, from a source
	// file named <name>.pb.ts, so those take their own, longer pattern.
	source := filepath.Join(library, js.Scriptc)
	index := filepath.Join(source, "js", "src", "index.ts")
	protobuf := filepath.Join(source, js.ScriptcProtobuf)
	config.CompilerOptions.Paths = map[string][]string{}
	for name, targets := range own {
		config.CompilerOptions.Paths[name] = targets
	}
	_, ownLibrary := own["modlock"]
	for name, targets := range map[string][]string{
		"modlock":                                   {index},
		"modlock/jsx-runtime":                       {index},
		"modlock/entities":                          {filepath.Join(source, "js", "src", "entities.ts")},
		"modlock/console":                           {filepath.Join(source, "js", "src", "console.ts")},
		"modlock/scriptc":                           {filepath.Join(source, "js", "src", "scriptc.ts")},
		"@aptre/protobuf-es-lite":                   {filepath.Join(protobuf, "index.ts")},
		"@aptre/protobuf-es-lite/*":                 {filepath.Join(protobuf, "*.ts")},
		"@aptre/protobuf-es-lite/google/protobuf/*": {filepath.Join(protobuf, "google", "protobuf", "*.pb.ts")},
	} {
		if ownLibrary && isLibrary(name) {
			continue
		}
		config.CompilerOptions.Paths[name] = targets
	}

	// Write it.
	data, err := json.MarshalIndent(config, "", "  ")
	if err != nil {
		return err
	}
	return os.WriteFile(filepath.Join(dir, "tsconfig.json"), data, 0o644)
}

// compileScriptc compiles the project with scriptc into a module fitted to
// the mod boundary. tsgo is the type checker, which lists the program's
// files. The compiler's diagnostics go to output; they list what keeps a mod
// from compiling.
//
// scriptc compiles a mirror of the program's sources outside any
// node_modules, with each TSX file's JSX lowered to calls: scriptc compiles
// no JSX, and it judges a bare import as an npm package whenever a
// node_modules beside the source holds its declarations, even when the
// configuration maps it to source.
func (p *Project) compileScriptc(ctx context.Context, library, tsgo string, output io.Writer) ([]byte, error) {
	// Install the compiler.
	compiler, runtimePack, err := scriptc.Compiler(ctx)
	if err != nil {
		return nil, err
	}

	// Make the mirror's directory, named as scriptc resolves it.
	mirror, err := os.MkdirTemp("", "modlock-scriptc-")
	if err != nil {
		return nil, errors.Wrap(err, "mirror the mod's sources")
	}
	defer os.RemoveAll(mirror)
	if mirror, err = filepath.EvalSymlinks(mirror); err != nil {
		return nil, errors.Wrap(err, "mirror the mod's sources")
	}

	// Mirror the program's sources, and write the entry beside the mirrored
	// project.
	project, sources, err := p.mirrorSources(ctx, tsgo, mirror)
	if err != nil {
		return nil, err
	}
	own, err := p.projectPaths(ctx, tsgo, project)
	if err != nil {
		return nil, err
	}
	dir := filepath.Join(project, scriptcEntryDir)
	if err := p.writeScriptcEntry(dir, library, own); err != nil {
		return nil, err
	}

	// Compile a WASI reactor in the entry's directory. scriptc's cycle
	// collector now and then crashes the compiler on a program the next run
	// compiles, so a run that a signal ends is tried again, up to three runs.
	var diagnostics bytes.Buffer
	for attempt := 1; ; attempt++ {
		diagnostics.Reset()
		build := exec.CommandContext(ctx, compiler, "build", "--lib", "--profile", "profile.json", "-o", Entry)
		build.Dir = dir
		build.Env = append(os.Environ(), "SCRIPTC_TARGET=wasm32-wasi", "SCRIPTC_RUNTIME_PACK="+runtimePack)
		build.Stderr = &diagnostics
		err = build.Run()
		if attempt == 3 || !crashed(ctx, err) {
			break
		}
		_, _ = fmt.Fprintf(output, "scriptc crashed (%v); compiling again\n", err)
	}

	// Print what keeps the mod from compiling, naming the sources, and name
	// the first error in the returned one.
	named := sources.Replace(diagnostics.String())
	_, _ = io.WriteString(output, named)
	if err != nil {
		if first := firstScriptcError(named); first != "" {
			return nil, errors.Errorf("scriptc cannot compile the mod: %s", first)
		}
		return nil, errors.Wrap(err, "scriptc cannot compile the mod")
	}

	// Fit the reactor to the mod boundary.
	built, err := os.ReadFile(filepath.Join(dir, Entry))
	if err != nil {
		return nil, errors.Wrap(err, "read the scriptc build")
	}
	fitted, err := scriptc.Fit(built)
	if err != nil {
		return nil, err
	}

	// Optimize the module, keeping the fitted one if wasm-opt cannot run.
	optimized, err := binaryen.Optimize(ctx, fitted)
	if err != nil {
		_, _ = fmt.Fprintf(output, "%v; the module is not optimized\n", err)
		return fitted, nil
	}
	return optimized, nil
}

// writeScriptcEntry writes the build's entry, its profile and its
// configuration into dir, mapping the library to its source in library.
func (p *Project) writeScriptcEntry(dir, library string, own map[string][]string) error {
	// Write the entry, which loads the project's main module.
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return errors.Wrap(err, "write the scriptc entry")
	}
	entry := fmt.Appendf(nil, scriptcEntry, p.scriptEntry())
	if err := os.WriteFile(filepath.Join(dir, "entry.ts"), entry, 0o644); err != nil {
		return errors.Wrap(err, "write the scriptc entry")
	}

	// Write the profile and the configuration that build it.
	if err := os.WriteFile(filepath.Join(dir, "profile.json"), scriptc.Profile("entry.ts"), 0o644); err != nil {
		return errors.Wrap(err, "write the scriptc profile")
	}
	if err := p.writeScriptcConfig(dir, library, own); err != nil {
		return errors.Wrap(err, "write the scriptc configuration")
	}
	return nil
}

// projectPaths returns the project's own path mappings, such as one that
// stands a file in for a package, with their targets in the mirrored project
// at mirror. The protobuf runtime's mappings are the build's, and so are the
// library's unless the project maps the library to TypeScript source, as the
// library's own tests do; a mod maps it to the installed declarations.
func (p *Project) projectPaths(ctx context.Context, tsgo, mirror string) (map[string][]string, error) {
	// Read the project's configuration as tsgo resolves it.
	show := exec.CommandContext(ctx, tsgo, "--project", p.Dir, "--showConfig")
	show.Dir = p.Dir
	shown, err := show.Output()
	if err != nil {
		return nil, errors.Wrap(err, "read the project's configuration")
	}
	var config struct {
		CompilerOptions struct {
			Paths map[string][]string `json:"paths"`
		} `json:"compilerOptions"`
	}
	if err := json.Unmarshal(shown, &config); err != nil {
		return nil, errors.Wrap(err, "read the project's configuration")
	}

	// Keep the mappings the build does not make, aimed at the mirror.
	library := config.CompilerOptions.Paths["modlock"]
	fromSource := len(library) != 0 && !slices.ContainsFunc(library, func(target string) bool {
		return !strings.HasSuffix(target, ".ts") || strings.HasSuffix(target, ".d.ts")
	})
	own := map[string][]string{}
	for name, targets := range config.CompilerOptions.Paths {
		if (isLibrary(name) && !fromSource) || strings.HasPrefix(name, "@aptre/protobuf-es-lite") {
			continue
		}
		for _, target := range targets {
			own[name] = append(own[name], filepath.Join(mirror, filepath.FromSlash(target)))
		}
	}
	return own, nil
}

// mirrorSources copies the program's sources into mirror at their paths
// below the directory that holds them all and the project, lowering each TSX
// file into a TypeScript file. The program is what tsgo lists, less the
// files in node_modules and tsgo's own. It returns the project's directory in
// the mirror and a replacer that names mirrored files by their sources.
func (p *Project) mirrorSources(ctx context.Context, tsgo, mirror string) (string, *strings.Replacer, error) {
	// List the program's own files.
	list := exec.CommandContext(ctx, tsgo, "--project", p.Dir, "--listFilesOnly")
	list.Dir = p.Dir
	listed, err := list.Output()
	if err != nil {
		return "", nil, errors.Wrap(err, "list the mod's sources")
	}
	tsgoDir := filepath.Dir(tsgo)
	var files []string
	for line := range strings.Lines(string(listed)) {
		line = strings.TrimSpace(line)
		file := filepath.Clean(filepath.FromSlash(line))
		if line == "" || inDir(file, tsgoDir) || slices.Contains(strings.Split(file, string(filepath.Separator)), "node_modules") {
			continue
		}
		files = append(files, file)
	}

	// Find the directory that holds them and the project.
	project, err := filepath.Abs(p.Dir)
	if err != nil {
		return "", nil, errors.Wrap(err, "mirror the mod's sources")
	}
	root := project
	for _, file := range files {
		for !inDir(file, root) {
			root = filepath.Dir(root)
		}
	}

	// Copy each file, lowering TSX. A replacer pair maps each lowered file
	// back to its source ahead of the pair for the whole mirror.
	var names []string
	for _, file := range files {
		rel, err := filepath.Rel(root, file)
		if err != nil {
			return "", nil, errors.Wrap(err, "mirror the mod's sources")
		}
		data, err := os.ReadFile(file)
		if err != nil {
			return "", nil, errors.Wrap(err, "mirror the mod's sources")
		}
		if strings.HasSuffix(rel, ".tsx") {
			if data, err = scriptc.LowerJSX(data); err != nil {
				return "", nil, errors.Wrapf(err, "lower the JSX in %s", file)
			}
			rel = strings.TrimSuffix(rel, "x")
			names = append(names, filepath.Join(mirror, rel), file)
		}
		target := filepath.Join(mirror, rel)
		if err := os.MkdirAll(filepath.Dir(target), 0o755); err != nil {
			return "", nil, errors.Wrap(err, "mirror the mod's sources")
		}
		if err := os.WriteFile(target, data, 0o644); err != nil {
			return "", nil, errors.Wrap(err, "mirror the mod's sources")
		}
	}
	names = append(names, mirror, root)

	// Locate the project in the mirror.
	rel, err := filepath.Rel(root, project)
	if err != nil {
		return "", nil, errors.Wrap(err, "mirror the mod's sources")
	}
	return filepath.Join(mirror, rel), strings.NewReplacer(names...), nil
}

// isLibrary reports whether the module name names the library or one of its
// modules.
func isLibrary(name string) bool {
	return name == "modlock" || strings.HasPrefix(name, "modlock/")
}

// isFile reports whether path names a file.
func isFile(path string) bool {
	info, err := os.Stat(path)
	return err == nil && !info.IsDir()
}

// inDir reports whether path is dir or lies below it.
func inDir(path, dir string) bool {
	rel, err := filepath.Rel(dir, path)
	return err == nil && rel != ".." && !strings.HasPrefix(rel, ".."+string(filepath.Separator))
}

// firstScriptcError returns the first error line of scriptc's diagnostics,
// such as "main.ts:3:54 - error SC2020: ...", or "" when they hold none.
func firstScriptcError(diagnostics string) string {
	for line := range strings.Lines(diagnostics) {
		if strings.Contains(line, " - error ") {
			return strings.TrimSpace(line)
		}
	}
	return ""
}

// crashed reports whether err is a run of a program that a signal ended while
// ctx was live; canceling ctx kills the program with a signal too.
func crashed(ctx context.Context, err error) bool {
	var exit *exec.ExitError
	return ctx.Err() == nil && errors.As(err, &exit) && exit.ExitCode() == -1
}
