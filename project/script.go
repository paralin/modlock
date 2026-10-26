package project

import (
	"bytes"
	"context"
	"fmt"
	"io"
	"io/fs"
	"os"
	"os/exec"
	"path/filepath"
	"strings"

	"github.com/evanw/esbuild/pkg/api"
	"github.com/paralin/modlock/internal/fileretry"
	"github.com/paralin/modlock/internal/npm"
	"github.com/paralin/modlock/js"
	"github.com/paralin/modlock/src/wasm/manifest"
	"github.com/pkg/errors"
)

// ScriptEntry is the bundled JavaScript's file name in a built mod.
const ScriptEntry = "mod.js"

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
func scriptMain(language manifest.Manifest_Language) string {
	if language == manifest.Manifest_LANGUAGE_JAVASCRIPT {
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
// of a mod that asks for RUNTIME_WASM compiles it with scriptc; any other
// build, or a mod scriptc cannot compile, bundles it with the library into
// one script for QuickJS.
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

	// Compile a release build with scriptc when the mod asks and scriptc can.
	if mode == Release && p.Manifest.GetRuntime() == manifest.Manifest_RUNTIME_WASM {
		module, err := p.compileScriptc(ctx, library, tsgo, output)
		if err == nil {
			return p.writeBuild(module, manifest.Manifest_RUNTIME_WASM, Entry, ScriptEntry)
		}
		_, _ = fmt.Fprintf(output, "%v; the mod runs on QuickJS\n", err)
	}

	// Bundle the sources and the library into one script.
	result := api.Build(api.BuildOptions{
		EntryPoints:     []string{p.scriptEntry()},
		AbsWorkingDir:   p.Dir,
		Bundle:          true,
		Format:          api.FormatIIFE,
		Platform:        api.PlatformNeutral,
		MainFields:      []string{"module", "main"},
		Target:          api.ES2023,
		JSX:             api.JSXAutomatic,
		JSXImportSource: "modlock",
		Alias:           map[string]string{"modlock": library},
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

	return p.writeBuild(result.OutputFiles[0].Contents, manifest.Manifest_RUNTIME_QUICKJS, ScriptEntry, Entry)
}

// writeBuild replaces the built mod's entry with content, then the manifest
// that names it, and removes the entry of the other runtime, stale, a
// script mod builds for.
func (p *Project) writeBuild(content []byte, runtime manifest.Manifest_Runtime, entry, stale string) error {
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
