package js_test

import (
	"bytes"
	"context"
	"encoding/json"
	"os"
	"os/exec"
	"path"
	"path/filepath"
	"strings"
	"testing"

	"github.com/paralin/modlock/project"
	"github.com/paralin/modlock/sandbox"
	manifestpb "github.com/paralin/modlock/src/wasm/manifest"
)

// suiteDir is the scriptc suite's directory: the bun:test stand-in, the
// configuration and the manifest each test file's project copies.
var suiteDir = filepath.Join("test", "scriptc")

// pending names the test files that scriptc cannot compile yet, with the
// reason. entities.create is generic over a designer name, which scriptc
// widens to string, so no type indexed by the name compiles.
var pending = map[string]string{
	"host": "entities.create, keyed by designer name, does not compile under scriptc",
}

// suiteBuilds is how many test files build at once. Each scriptc build takes
// about three minutes and one and a half gigabytes.
const suiteBuilds = 3

// TestScriptcSuite compiles each of the library's test files with scriptc and
// runs it as WebAssembly, so a library construct that scriptc rejects or
// miscompiles fails here rather than in a mod. Each file is its own module:
// the files bind the host at their top level, and one file's bind and the
// library state it leaves would otherwise reach the next.
func TestScriptcSuite(t *testing.T) {
	if testing.Short() {
		t.Skip("each test file takes minutes to compile with scriptc")
	}
	if _, err := exec.LookPath("zig"); err != nil {
		t.Skip("scriptc links with zig, which is not on PATH")
	}

	// Read the suite's configuration and list the test files.
	config, err := os.ReadFile(filepath.Join(suiteDir, "tsconfig.json"))
	if err != nil {
		t.Fatal(err)
	}
	files, err := filepath.Glob(filepath.Join("test", "*.test.ts"))
	if err != nil || len(files) == 0 {
		t.Fatalf("list the test files: %v", err)
	}

	// Build and run each file, a few at a time.
	slots := make(chan struct{}, suiteBuilds)
	for _, file := range files {
		name := strings.TrimSuffix(filepath.Base(file), ".test.ts")
		t.Run(name, func(t *testing.T) {
			if reason, ok := pending[name]; ok {
				t.Skip(reason)
			}
			t.Parallel()
			slots <- struct{}{}
			defer func() { <-slots }()
			runSuiteFile(t, config, name)
		})
	}
}

// runSuiteFile builds the test file name as a project of its own under the
// suite's build directory and runs it, failing on each failed test. A build
// scriptc cannot compile falls back to QuickJS, so it checks that the build
// is WebAssembly.
func runSuiteFile(t *testing.T, config []byte, name string) {
	ctx := context.Background()

	// Write the file's project two directories below the suite.
	dir := filepath.Join(suiteDir, project.BuildDir, name)
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	rebased, err := rebaseConfig(config, "../..")
	if err != nil {
		t.Fatal(err)
	}
	manifest, err := os.ReadFile(filepath.Join(suiteDir, "mod.json"))
	if err != nil {
		t.Fatal(err)
	}
	main := "import { run } from '../../testing.js'\nimport '../../../" + name + ".test.js'\n\nrun()\n"
	for file, data := range map[string][]byte{"tsconfig.json": rebased, "mod.json": manifest, "main.ts": []byte(main)} {
		if err := os.WriteFile(filepath.Join(dir, file), data, 0o644); err != nil {
			t.Fatal(err)
		}
	}

	// Build it, keeping the compiler's output for a failure.
	suite, err := project.Open(dir)
	if err != nil {
		t.Fatal(err)
	}
	var output bytes.Buffer
	if err := suite.Build(ctx, &output, project.Release); err != nil {
		t.Fatalf("build: %v\n%s", err, output.String())
	}
	built, err := project.ReadManifest(suite.Output())
	if err != nil {
		t.Fatal(err)
	}
	if runtime := built.GetRuntime(); runtime != manifestpb.Manifest_RUNTIME_WASM {
		t.Fatalf("built for %s, not WebAssembly:\n%s", runtime, output.String())
	}
	module, err := os.ReadFile(filepath.Join(suite.Output(), project.Entry))
	if err != nil {
		t.Fatal(err)
	}

	// Run it: initialization runs every test and prints a line for each.
	var printed bytes.Buffer
	mod, err := sandbox.Load(ctx, module, sandbox.Options{Output: &printed, Interpret: true})
	if err != nil {
		t.Fatalf("run: %v\n%s", err, printed.String())
	}
	defer mod.Close(ctx)

	// Fail on each failed test and on a run that never finished.
	report := printed.String()
	finished := false
	for line := range strings.Lines(report) {
		line = strings.TrimSpace(line)
		switch {
		case strings.HasPrefix(line, "FAIL "):
			t.Error(line)
		case strings.HasSuffix(line, " failed"):
			finished = true
			t.Log(line)
		}
	}
	if !finished {
		t.Errorf("the tests did not finish:\n%s", report)
	}
}

// rebaseConfig returns the TypeScript configuration config for a project in
// a directory up below it: each path mapping and include gains the prefix
// up, and the project's main.ts joins the include.
func rebaseConfig(config []byte, up string) ([]byte, error) {
	var parsed struct {
		CompilerOptions map[string]json.RawMessage `json:"compilerOptions"`
		Include         []string                   `json:"include"`
		Exclude         []string                   `json:"exclude"`
	}
	if err := json.Unmarshal(config, &parsed); err != nil {
		return nil, err
	}

	// Rebase the mappings and the include.
	var paths map[string][]string
	if err := json.Unmarshal(parsed.CompilerOptions["paths"], &paths); err != nil {
		return nil, err
	}
	for _, targets := range paths {
		for i, target := range targets {
			targets[i] = path.Join(up, target)
		}
	}
	include := []string{"main.ts"}
	for _, pattern := range parsed.Include {
		include = append(include, path.Join(up, pattern))
	}

	// Write the configuration back.
	encoded, err := json.Marshal(paths)
	if err != nil {
		return nil, err
	}
	parsed.CompilerOptions["paths"] = encoded
	parsed.Include = include
	return json.MarshalIndent(parsed, "", "  ")
}
