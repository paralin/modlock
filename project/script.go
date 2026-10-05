package project

import (
	"bytes"
	"context"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strings"

	"github.com/evanw/esbuild/pkg/api"
	"github.com/paralin/modlock/internal/npm"
	"github.com/paralin/modlock/js"
	"github.com/paralin/modlock/proto/modlock/wasm"
	"github.com/pkg/errors"
)

// ScriptEntry is the bundled JavaScript's file name in a built mod.
const ScriptEntry = "mod.js"

// libraryDir is where a script project's copy of the modlock library lives,
// so the type checker, the bundler and editors resolve imports of modlock.
var libraryDir = filepath.Join("node_modules", "modlock")

// libraryPackage is the library's package.json.
const libraryPackage = `{
  "name": "modlock",
  "type": "module",
  "main": "index.js",
  "types": "index.d.ts",
  "sideEffects": false
}
`

// jsxRuntime and jsxTypes are the library's JSX runtime, which the compiler
// and the bundler import as modlock/jsx-runtime.
const (
	jsxRuntime = "export { Fragment, jsx, jsxs } from './index.js'\n"
	jsxTypes   = "export { Fragment, JSX, jsx, jsxs } from './index.js'\n"
)

// scriptMain returns the source entry of a TypeScript or JavaScript project.
func scriptMain(language wasm.Manifest_Language) string {
	if language == wasm.Manifest_LANGUAGE_JAVASCRIPT {
		return "main.js"
	}
	return "main.ts"
}

// buildScript type checks a TypeScript or JavaScript project and bundles it
// with the library into one script for QuickJS.
func (p *Project) buildScript(ctx context.Context, output io.Writer) error {
	// Install the library and the type checker.
	if err := installLibrary(ctx, p.Dir); err != nil {
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

	// Bundle the sources and the library into one script.
	result := api.Build(api.BuildOptions{
		EntryPoints:     []string{scriptMain(p.Manifest.GetLanguage())},
		AbsWorkingDir:   p.Dir,
		Bundle:          true,
		Format:          api.FormatIIFE,
		Platform:        api.PlatformNeutral,
		MainFields:      []string{"module", "main"},
		Target:          api.ES2023,
		JSX:             api.JSXAutomatic,
		JSXImportSource: "modlock",
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

	// Replace the script, then the manifest that names it.
	if err := os.MkdirAll(p.Output(), 0o755); err != nil {
		return errors.Wrap(err, "create the build directory")
	}
	staged := filepath.Join(p.Output(), ScriptEntry+".tmp")
	if err := os.WriteFile(staged, result.OutputFiles[0].Contents, 0o644); err != nil {
		return errors.Wrap(err, "write the script")
	}
	if err := os.Rename(staged, filepath.Join(p.Output(), ScriptEntry)); err != nil {
		return errors.Wrap(err, "write the script")
	}
	return p.writeBuiltManifest(wasm.Manifest_RUNTIME_QUICKJS, ScriptEntry)
}

// installLibrary copies the built library into the project in dir, leaving
// files that already match alone.
func installLibrary(ctx context.Context, dir string) error {
	// Gather the library's files and the package that names them.
	library, err := js.Library(ctx)
	if err != nil {
		return err
	}
	files := map[string][]byte{
		"package.json":     []byte(libraryPackage),
		"jsx-runtime.js":   []byte(jsxRuntime),
		"jsx-runtime.d.ts": []byte(jsxTypes),
	}
	for _, name := range js.Files {
		content, err := os.ReadFile(filepath.Join(library, name))
		if err != nil {
			return errors.Wrap(err, "install the modlock library")
		}
		files[name] = content
	}

	// Write each file that differs.
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
	return nil
}
