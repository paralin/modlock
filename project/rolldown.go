package project

import (
	"bufio"
	"bytes"
	"context"
	_ "embed"
	"encoding/json"
	"fmt"
	"io"
	"os/exec"
	"path/filepath"
	"runtime"

	"github.com/paralin/modlock/cli"
	"github.com/paralin/modlock/internal/npm"
	"github.com/paralin/modlock/python"
	"github.com/pkg/errors"
)

// RolldownVersion is the Rolldown release that bundles and minifies a script
// mod's release build.
const RolldownVersion = "1.2.15"

// rolldownPackages are the packages Rolldown imports at run time, beside its
// platform's binding, at the versions RolldownVersion asks for.
var rolldownPackages = map[string]string{
	"rolldown":              RolldownVersion,
	"@oxc-project/types":    "0.155.0",
	"@rolldown/pluginutils": "1.0.1",
}

// rolldownBindings maps each system and architecture to its Rolldown binding.
var rolldownBindings = map[string]string{
	"darwin/amd64":  "darwin-x64",
	"darwin/arm64":  "darwin-arm64",
	"linux/amd64":   "linux-x64-gnu",
	"linux/arm64":   "linux-arm64-gnu",
	"windows/amd64": "win32-x64-msvc",
	"windows/arm64": "win32-arm64-msvc",
}

// rolldownScript bundles a mod with Rolldown on Node.
//
//go:embed rolldown.ts
var rolldownScript string

// rolldownError is one error rolldownScript reports.
type rolldownError struct {
	Message string `json:"message"`
	File    string `json:"file"`
	Line    uint32 `json:"line"`
	Column  uint32 `json:"column"`
}

// bundleRolldown bundles the project's sources and the library into one
// minified script for QuickJS, writing any bundling errors to output.
func (p *Project) bundleRolldown(ctx context.Context, library string, output io.Writer) ([]byte, error) {
	// Install Node and Rolldown with this system's binding.
	binding, ok := rolldownBindings[runtime.GOOS+"/"+runtime.GOARCH]
	if !ok {
		return nil, errors.Errorf("Rolldown publishes no binding for %s/%s", runtime.GOOS, runtime.GOARCH)
	}
	node, err := python.Node(ctx)
	if err != nil {
		return nil, err
	}
	packages := map[string]string{"@rolldown/binding-" + binding: RolldownVersion}
	for name, version := range rolldownPackages {
		packages[name] = version
	}
	modules, err := npm.Modules(ctx, packages)
	if err != nil {
		return nil, errors.Wrap(err, "install Rolldown")
	}

	// Bundle the mod.
	options, err := json.Marshal(map[string]string{
		"rolldown": filepath.Join(modules, "node_modules", "rolldown", "dist", "index.mjs"),
		"dir":      p.Dir,
		"entry":    p.scriptEntry(),
		"library":  library,
		"styles":   filepath.Join(p.Dir, ContentDir, "styles"),
	})
	if err != nil {
		return nil, err
	}
	var bundled, stderr bytes.Buffer
	bundle := exec.CommandContext(ctx, node, "--input-type=module-typescript", "--eval", rolldownScript, string(options))
	bundle.Dir = p.Dir
	bundle.Stdout = &bundled
	bundle.Stderr = &stderr
	err = bundle.Run()
	var exit *exec.ExitError
	if err == nil {
		return bundled.Bytes(), nil
	}
	if !errors.As(err, &exit) || exit.ExitCode() != 2 {
		return nil, errors.Errorf("run Rolldown: %v: %s", err, stderr.Bytes())
	}

	// Print and locate each bundling error.
	var diagnostics []*cli.Diagnostic
	lines := bufio.NewScanner(&bundled)
	for lines.Scan() {
		var found rolldownError
		if json.Unmarshal(lines.Bytes(), &found) != nil {
			continue
		}
		diagnostic := &cli.Diagnostic{Message: found.Message, Source: "rolldown"}
		if found.File != "" {
			diagnostic.File = absolute(p.Dir, found.File)
			diagnostic.Line = found.Line
			diagnostic.Column = found.Column + 1
			_, _ = fmt.Fprintf(output, "%s:%d:%d: ", diagnostic.File, diagnostic.Line, diagnostic.Column)
		}
		diagnostics = append(diagnostics, diagnostic)
		_, _ = fmt.Fprintln(output, found.Message)
	}
	return nil, &CheckError{Err: errors.New("the mod does not bundle"), Diagnostics: diagnostics}
}
