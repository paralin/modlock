package project

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"os/exec"
	"path"
	"slices"

	"github.com/paralin/modlock/proto/modlock/cli"
	"github.com/paralin/modlock/proto/modlock/wasm"
	"github.com/paralin/modlock/python"
	"github.com/pkg/errors"
)

// pythonConfig is a Python project's pyrightconfig.json, which the checker
// and editors read: strict checking against the runtime's Python, with the
// installed library on the import path.
const pythonConfig = `{
  "typeCheckingMode": "strict",
  "pythonVersion": "3.14",
  "extraPaths": [".modlock"],
  "reportMissingModuleSource": false,
  "exclude": ["build", ".modlock"]
}
`

// buildPython type checks a Python project and zips its sources with the
// library for the Python runtime.
func (p *Project) buildPython(ctx context.Context, output io.Writer) error {
	// Install the library and the checker, then gather the sources.
	if err := installTree(p.Dir, python.Library); err != nil {
		return err
	}
	node, pyright, err := python.Checker(ctx)
	if err != nil {
		return err
	}
	sources, err := sourceFiles(p.Dir, ".py")
	if err != nil {
		return err
	}
	if !slices.Contains(sources, "main.py") {
		return errors.New("the project has no main.py")
	}

	// Check the types, locating each error.
	var report bytes.Buffer
	check := exec.CommandContext(ctx, node, pyright, "--outputjson")
	check.Dir = p.Dir
	check.Stdout = &report
	check.Stderr = output
	err = check.Run()
	diagnostics := pyrightDiagnostics(report.Bytes())
	for _, d := range diagnostics {
		fmt.Fprintf(output, "%s:%d:%d: %s (%s)\n", d.GetFile(), d.GetLine(), d.GetColumn(), d.GetMessage(), d.GetCode())
	}
	if err != nil {
		return &CheckError{Err: errors.New("the mod has type errors"), Diagnostics: diagnostics}
	}

	// Zip the sources with the library's package; the runtime builds in its
	// bridge, so the stub stays out.
	return p.writeSources(wasm.Manifest_RUNTIME_PYTHON, sources, python.Library, func(name string) string {
		if path.Ext(name) != ".py" {
			return ""
		}
		return name
	})
}

// pyrightReport is the part of Pyright's --outputjson report that locates
// problems. Lines and characters count from zero.
type pyrightReport struct {
	GeneralDiagnostics []struct {
		File     string `json:"file"`
		Severity string `json:"severity"`
		Message  string `json:"message"`
		Rule     string `json:"rule"`
		Range    struct {
			Start struct {
				Line      uint32 `json:"line"`
				Character uint32 `json:"character"`
			} `json:"start"`
		} `json:"range"`
	} `json:"generalDiagnostics"`
}

// pyrightDiagnostics reads Pyright's JSON report. It leaves out
// informational notes.
func pyrightDiagnostics(output []byte) []*cli.Diagnostic {
	var report pyrightReport
	if json.Unmarshal(output, &report) != nil {
		return nil
	}
	var found []*cli.Diagnostic
	for _, d := range report.GeneralDiagnostics {
		if d.Severity == "information" {
			continue
		}
		found = append(found, &cli.Diagnostic{
			File:    d.File,
			Line:    d.Range.Start.Line + 1,
			Column:  d.Range.Start.Character + 1,
			Message: d.Message,
			Source:  "pyright",
			Code:    d.Rule,
		})
	}
	return found
}
