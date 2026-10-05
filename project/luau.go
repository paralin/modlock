package project

import (
	"bytes"
	"context"
	"io"
	"os/exec"
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

	// Zip the sources with the library under @modlock.
	return p.writeSources(wasm.Manifest_RUNTIME_LUAU, sources, luau.Library, func(name string) string {
		return "@" + name
	})
}
