package project

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"io/fs"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strings"

	"github.com/evanw/esbuild/pkg/api"
	"github.com/paralin/modlock/internal/fileretry"
	"github.com/paralin/modlock/internal/npm"
	"github.com/paralin/modlock/js"
	"github.com/paralin/modlock/src/wasm"
	"github.com/pkg/errors"
)

// ScriptEntry is the bundled JavaScript's file name in a built mod.
const ScriptEntry = "mod.js"

// SourceMapEntry is the file beside a dev build's script that maps its lines
// to the mod's sources, which the session uses to name the source line of a
// slow frame. Other builds hold none.
const SourceMapEntry = ScriptEntry + ".map"

// libraryDir is where a script project's copy of the modlock library's
// declarations lives, so the type checker and editors resolve imports of
// modlock. The compilers resolve modlock to the built library instead.
var libraryDir = filepath.Join("node_modules", "modlock")

// libraryPackage is the declarations' package.json.
const libraryPackage = `{
  "name": "modlock",
  "type": "module",
  "types": "index.d.ts",
  "sideEffects": false
}
`

// scriptMain returns the source entry of a TypeScript or JavaScript project.
func scriptMain(language wasm.Manifest_Language) string {
	if language == wasm.Manifest_LANGUAGE_JAVASCRIPT {
		return "main.js"
	}
	return "main.ts"
}

// scriptEntry returns the project's source entry: scriptMain, or its JSX
// form, such as main.tsx, when the project has that instead.
func (p *Project) scriptEntry() string {
	main := scriptMain(p.Manifest.GetLanguage())
	if _, err := os.Stat(filepath.Join(p.Dir, main+"x")); err == nil {
		return main + "x"
	}
	return main
}

// buildScript type checks a TypeScript or JavaScript project. A release build
// of a mod that asks for RUNTIME_WASM compiles it with scriptc and ships the
// module once checkTwin finds it behaves as the QuickJS bundle does; any other
// build, or a module scriptc cannot compile or that differs, bundles it with
// the library into one script for QuickJS: a release with Rolldown, minified,
// and a dev build with esbuild, which needs no download and keeps the script
// readable.
func (p *Project) buildScript(ctx context.Context, output io.Writer, mode Mode) error {
	// Install the library's declarations and the type checker.
	library, err := js.Library(ctx)
	if err != nil {
		return err
	}
	if err := installLibrary(library, p.Dir); err != nil {
		return err
	}
	tsgo, err := npm.Tsgo(ctx)
	if err != nil {
		return err
	}

	// Check the types, locating each error.
	var printed bytes.Buffer
	check := exec.CommandContext(ctx, tsgo, "--project", p.Dir, "--pretty", "false")
	check.Dir = p.Dir
	check.Stdout = io.MultiWriter(output, &printed)
	check.Stderr = check.Stdout
	if err := check.Run(); err != nil {
		return &CheckError{
			Err:         errors.New("the mod has type errors"),
			Diagnostics: tsgoDiagnostics(p.Dir, printed.Bytes()),
		}
	}

	// Bundle and minify a release with Rolldown, then compile it with scriptc
	// when the mod asks and scriptc can, shipping the module only when it
	// behaves as the bundle does. A mod that asked for WebAssembly still
	// builds, so it stays playable, and the build ends on a warning that
	// names why, below the diagnostics where it is seen.
	if mode == Release {
		bundled, err := p.bundleRolldown(ctx, library, output)
		if err != nil {
			return err
		}
		var fallback error
		if p.Manifest.GetRuntime() == wasm.Manifest_RUNTIME_WASM {
			module, err := p.compileScriptc(ctx, library, tsgo, output)
			if err == nil {
				err = p.checkTwin(ctx, output, module, bundled)
			}
			if err == nil {
				return p.writeBuild(module, nil, wasm.Manifest_RUNTIME_WASM, Entry, ScriptEntry)
			}
			fallback = err
		}
		if err := p.writeBuild(bundled, nil, wasm.Manifest_RUNTIME_QUICKJS, ScriptEntry, Entry); err != nil {
			return err
		}
		if fallback != nil {
			_, _ = fmt.Fprintf(output, "warning: the mod asks for RUNTIME_WASM, but %v, so this build runs on QuickJS\n", fallback)
		}
		return nil
	}

	// Bundle a dev build's sources and the library into one script, with a
	// map of its lines named from the build directory.
	result := api.Build(api.BuildOptions{
		EntryPoints:     []string{p.scriptEntry()},
		AbsWorkingDir:   p.Dir,
		Outfile:         filepath.Join(p.Output(), ScriptEntry),
		Sourcemap:       api.SourceMapExternal,
		SourcesContent:  api.SourcesContentExclude,
		Bundle:          true,
		Format:          api.FormatIIFE,
		Platform:        api.PlatformNeutral,
		MainFields:      []string{"module", "main"},
		Target:          api.ES2023,
		JSX:             api.JSXAutomatic,
		JSXImportSource: "modlock",
		Alias:           map[string]string{"modlock": library},
		Plugins:         []api.Plugin{p.sheetPlugin()},
		LogLevel:        api.LogLevelSilent,
	})

	// Print and locate each bundling error.
	messages := api.FormatMessages(result.Errors, api.FormatMessagesOptions{Kind: api.ErrorMessage})
	if len(messages) != 0 {
		_, _ = io.WriteString(output, strings.Join(messages, ""))
		return &CheckError{
			Err:         errors.New("the mod does not bundle"),
			Diagnostics: esbuildDiagnostics(p.Dir, result.Errors),
		}
	}

	var script, sourceMap []byte
	for _, file := range result.OutputFiles {
		if strings.HasSuffix(file.Path, ".map") {
			sourceMap = file.Contents
		} else {
			script = file.Contents
		}
	}
	return p.writeBuild(script, sourceMap, wasm.Manifest_RUNTIME_QUICKJS, ScriptEntry, Entry)
}

// sheetNamespace is the esbuild namespace of a style sheet import.
const sheetNamespace = "modlock-sheet"

// sheetPart matches one segment of a style sheet's path, as the library's
// sheet call accepts it.
var sheetPart = regexp.MustCompile(`^[\w.-]+$`)

// sheetPath returns the path, under the project's content/styles/ directory,
// of the style sheet at file, as the library's sheet call names it, or why a
// script cannot import file as a sheet.
func (p *Project) sheetPath(file string) (string, error) {
	// Compare the real paths, since the bundler may name either through a
	// symbolic link.
	styles := filepath.Join(p.Dir, ContentDir, "styles")
	real, err := filepath.EvalSymlinks(file)
	if err != nil {
		return "", errors.Errorf("no style sheet at %s", filepath.Base(file))
	}
	if styles, err = filepath.EvalSymlinks(styles); err != nil {
		return "", errors.Errorf("a style sheet goes in %s/styles/, which the build compiles into the mod's content", ContentDir)
	}

	// Name the sheet by its path under the styles directory.
	rest, err := filepath.Rel(styles, real)
	if err != nil || !filepath.IsLocal(rest) {
		return "", errors.Errorf("a style sheet goes in %s/styles/, which the build compiles into the mod's content", ContentDir)
	}
	rest = filepath.ToSlash(rest)
	for _, part := range strings.Split(rest, "/") {
		if !sheetPart.MatchString(part) {
			return "", errors.Errorf("a style sheet's path is letters, digits, '_', '-' and '.': %s", rest)
		}
	}
	return rest, nil
}

// sheetModule is the script an import of the style sheet at path becomes: a
// call that adds the sheet to every interface the mod shows.
func sheetModule(path string) string {
	name, _ := json.Marshal(path)
	return "import { sheet } from 'modlock'\nsheet(" + string(name) + ")\n"
}

// sheetPlugin turns each import of a .css file into sheetModule, so
// import './content/styles/hud.css' adds the compiled sheet to the mod's
// interfaces. Rolldown's release build does the same in rolldown.ts.
func (p *Project) sheetPlugin() api.Plugin {
	return api.Plugin{Name: "modlock-sheet", Setup: func(build api.PluginBuild) {
		build.OnResolve(api.OnResolveOptions{Filter: `\.css$`}, func(args api.OnResolveArgs) (api.OnResolveResult, error) {
			file := args.Path
			if !filepath.IsAbs(file) {
				file = filepath.Join(args.ResolveDir, file)
			}
			path, err := p.sheetPath(file)
			return api.OnResolveResult{Path: path, Namespace: sheetNamespace}, err
		})
		build.OnLoad(api.OnLoadOptions{Filter: `.*`, Namespace: sheetNamespace}, func(args api.OnLoadArgs) (api.OnLoadResult, error) {
			contents := sheetModule(args.Path)
			return api.OnLoadResult{Contents: &contents, ResolveDir: p.Dir, Loader: api.LoaderJS}, nil
		})
	}}
}

// writeBuild replaces the built mod's entry with content and its source map
// with sourceMap, removing the map when sourceMap is nil, then the manifest
// that names the entry, and removes the entry of the other runtime, stale, a
// script mod builds for.
func (p *Project) writeBuild(content, sourceMap []byte, runtime wasm.Manifest_Runtime, entry, stale string) error {
	// Stage the entry so a failed write leaves the previous build in place.
	if err := os.MkdirAll(p.Output(), 0o755); err != nil {
		return errors.Wrap(err, "create the build directory")
	}
	staged := filepath.Join(p.Output(), entry+".tmp")
	if err := os.WriteFile(staged, content, 0o644); err != nil {
		return errors.Wrap(err, "write the build")
	}
	if err := fileretry.Rename(staged, filepath.Join(p.Output(), entry)); err != nil {
		return errors.Wrap(err, "write the build")
	}

	// Write or drop the map; a map that outlived its script would name the
	// wrong lines, and a missing one only leaves the bundle's.
	mapPath := filepath.Join(p.Output(), SourceMapEntry)
	if sourceMap == nil {
		if err := os.Remove(mapPath); err != nil && !errors.Is(err, fs.ErrNotExist) {
			return errors.Wrap(err, "remove the previous source map")
		}
	} else if err := os.WriteFile(mapPath, sourceMap, 0o644); err != nil {
		return errors.Wrap(err, "write the source map")
	}

	// Name it in the manifest, then drop the entry the manifest stopped naming.
	if err := p.writeBuiltManifest(runtime, entry); err != nil {
		return err
	}
	if err := os.Remove(filepath.Join(p.Output(), stale)); err != nil && !errors.Is(err, fs.ErrNotExist) {
		return errors.Wrap(err, "remove the previous build")
	}
	return nil
}

// installLibrary installs the declarations of the built library in library
// into the script project in dir, removing any other file there.
func installLibrary(library, dir string) error {
	// Gather the declarations, every .d.ts file of the library, and the
	// package that names them.
	files := map[string][]byte{"package.json": []byte(libraryPackage)}
	declarations, err := filepath.Glob(filepath.Join(library, "*.d.ts"))
	if err != nil {
		return errors.Wrap(err, "install the modlock library")
	}
	for _, path := range declarations {
		content, err := os.ReadFile(path)
		if err != nil {
			return errors.Wrap(err, "install the modlock library")
		}
		files[filepath.Base(path)] = content
	}

	// Write each file that differs, and remove the rest.
	installed := filepath.Join(dir, libraryDir)
	if err := os.MkdirAll(installed, 0o755); err != nil {
		return errors.Wrap(err, "install the modlock library")
	}
	for name, content := range files {
		path := filepath.Join(installed, name)
		if existing, err := os.ReadFile(path); err == nil && bytes.Equal(existing, content) {
			continue
		}
		if err := os.WriteFile(path, content, 0o644); err != nil {
			return errors.Wrap(err, "install the modlock library")
		}
	}
	entries, err := os.ReadDir(installed)
	if err != nil {
		return errors.Wrap(err, "install the modlock library")
	}
	for _, entry := range entries {
		if _, ok := files[entry.Name()]; !ok {
			if err := os.RemoveAll(filepath.Join(installed, entry.Name())); err != nil {
				return errors.Wrap(err, "install the modlock library")
			}
		}
	}
	return nil
}
