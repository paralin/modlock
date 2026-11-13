package project

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path"
	"path/filepath"
	"slices"
	"strings"

	"github.com/paralin/modlock/cli"
	"github.com/paralin/modlock/python"
	"github.com/paralin/modlock/src/wasm"
	"github.com/pkg/errors"
)

// pythonConfig is a Python project's pyrightconfig.json, which the checker
// and editors read: standard checking against the runtime's Python, with the
// installed library on the import path. A value that may be None and a
// parameter without an annotation are warnings, which the build prints and
// passes, since a mod often knows the value is there.
const pythonConfig = `{
  "typeCheckingMode": "standard",
  "pythonVersion": "3.14",
  "extraPaths": [".modlock"],
  "reportMissingModuleSource": false,
  "reportMissingParameterType": "warning",
  "reportOptionalMemberAccess": "warning",
  "reportOptionalSubscript": "warning",
  "reportOptionalIterable": "warning",
  "reportOptionalCall": "warning",
  "reportOptionalOperand": "warning",
  "reportOptionalContextManager": "warning",
  "exclude": ["build", ".modlock"]
}
`

// pythonConfigsBefore are the configurations earlier versions wrote. A
// project that still has one unchanged gets the current one on its next
// build; one its author edited is left alone.
var pythonConfigsBefore = []string{
	`{
  "typeCheckingMode": "strict",
  "pythonVersion": "3.14",
  "extraPaths": [".modlock"],
  "reportMissingModuleSource": false,
  "exclude": ["build", ".modlock"]
}
`,
	`{
  "typeCheckingMode": "strict",
  "pythonVersion": "3.14",
  "extraPaths": [".modlock"],
  "reportMissingModuleSource": false,
  "reportUnknownParameterType": false,
  "reportUnknownMemberType": false,
  "reportUnknownArgumentType": false,
  "reportUnknownVariableType": false,
  "exclude": ["build", ".modlock"]
}
`,
}

// refreshPythonConfig replaces a configuration an earlier version wrote, if
// the project still has it unchanged.
func refreshPythonConfig(dir string) error {
	// Leave a missing or edited configuration alone.
	file := filepath.Join(dir, "pyrightconfig.json")
	data, err := os.ReadFile(file)
	if err != nil || !slices.Contains(pythonConfigsBefore, strings.ReplaceAll(string(data), "\r\n", "\n")) {
		return nil
	}
	return os.WriteFile(file, []byte(pythonConfig), 0o644)
}

// buildPython type checks a Python project and zips its sources with the
// library for the Python runtime.
func (p *Project) buildPython(ctx context.Context, output io.Writer) error {
	// Install the library and the checker, then gather the sources.
	if err := installTree(p.Dir, python.Library); err != nil {
		return err
	}
	if err := refreshPythonConfig(p.Dir); err != nil {
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

	// Print each problem where an editor's terminal links it.
	diagnostics := pyrightDiagnostics(report.Bytes())
	unannotated := false
	for _, d := range diagnostics {
		fmt.Fprintf(output, "%s:%d:%d: %s (%s)\n", d.GetFile(), d.GetLine(), d.GetColumn(), d.GetMessage(), d.GetCode())
		unannotated = unannotated || d.GetCode() == "reportMissingParameterType"
	}

	// Python reads a handler's parameter types from its own annotations and
	// not from its decorator, so say what to write.
	if unannotated {
		fmt.Fprintln(output, "give each parameter its type, such as def frame(event: modlock.FrameEvent) -> None; each decorator's documentation shows its handler")
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
	// GeneralDiagnostics lists each problem with its file, rule and place.
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
// informational notes and marks each warning, which does not fail the build.
func pyrightDiagnostics(output []byte) []*cli.Diagnostic {
	// Read the report; output that is not one locates nothing.
	var report pyrightReport
	if json.Unmarshal(output, &report) != nil {
		return nil
	}

	// Keep each warning and error with its place, counted from one.
	var found []*cli.Diagnostic
	for _, d := range report.GeneralDiagnostics {
		if d.Severity == "information" {
			continue
		}
		message := d.Message
		if d.Severity == "warning" {
			message = "warning: " + message
		}
		found = append(found, &cli.Diagnostic{
			File:    d.File,
			Line:    d.Range.Start.Line + 1,
			Column:  d.Range.Start.Character + 1,
			Message: message,
			Source:  "pyright",
			Code:    d.Rule,
		})
	}
	return found
}
