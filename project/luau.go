package project

import (
	"bytes"
	"context"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"slices"

	"github.com/paralin/modlock/luau"
	"github.com/paralin/modlock/proto/modlock/wasm"
	"github.com/pkg/errors"
)

// luauConfig is a Luau project's .luaurc, which the analyzer and editors
// read: strict checking, and the @modlock alias for the installed library.
const luauConfig = `{
  "languageMode": "strict",
  "aliases": {
    "modlock": "./.modlock/modlock"
  }
}
`

// luauOptional maps each generated library module, the entity classes and
// the console, to its file in the library. Only the mods that require a
// module carry it.
var luauOptional = map[string]string{
	"@modlock/entities": "modlock/entities.luau",
	"@modlock/console":  "modlock/console.luau",
}

// buildLuau type checks a Luau project and zips its sources with the
// library for the Luau runtime.
func (p *Project) buildLuau(ctx context.Context, output io.Writer) error {
	// Install the library and the analyzer, then gather the sources.
	if err := installTree(p.Dir, luau.Library); err != nil {
		return err
	}
	analyzer, err := luau.Analyzer(ctx)
	if err != nil {
		return err
	}
	sources, err := sourceFiles(p.Dir, ".luau", ".lua")
	if err != nil {
		return err
	}
	if !slices.Contains(sources, "main.luau") && !slices.Contains(sources, "main.lua") {
		return errors.New("the project has no main.luau")
	}

	// Check the types, locating each error.
	var printed bytes.Buffer
	check := exec.CommandContext(ctx, analyzer, sources...)
	check.Dir = p.Dir
	check.Stdout = io.MultiWriter(output, &printed)
	check.Stderr = check.Stdout
	if err := check.Run(); err != nil {
		return &CheckError{
			Err:         errors.New("the mod has type errors"),
			Diagnostics: luauDiagnostics(p.Dir, printed.Bytes()),
		}
	}

	// Zip the sources with the library under @modlock, leaving out each
	// optional module unless a source requires it. A require names its
	// module literally, so finding the name finds every use.
	unused := map[string]bool{}
	for _, file := range luauOptional {
		unused[file] = true
	}
	for _, name := range sources {
		data, err := os.ReadFile(filepath.Join(p.Dir, filepath.FromSlash(name)))
		if err != nil {
			return errors.Wrap(err, "read the sources")
		}
		for module, file := range luauOptional {
			if bytes.Contains(data, []byte(module)) {
				delete(unused, file)
			}
		}
	}
	return p.writeSources(wasm.Manifest_RUNTIME_LUAU, sources, luau.Library, func(name string) string {
		if unused[name] {
			return ""
		}
		return "@" + name
	})
}
