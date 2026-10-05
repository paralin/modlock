package check

import (
	"context"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
	"testing/fstest"
)

// TestMod checks a Go mod that starts, one that never finishes starting and
// one that imports a function the host does not offer.
func TestMod(t *testing.T) {
	for _, test := range []struct {
		mod  string
		fail string
	}{
		{"hello", ""},
		{"loop", "initialize took longer"},
		{"escape", "env.open"},
	} {
		t.Run(test.mod, func(t *testing.T) {
			dist := fstest.MapFS{
				"mod.json": {Data: []byte(`{"slug": "` + test.mod + `", "version": "0.1.0", "runtime": "RUNTIME_WASM", "entry": "mod.wasm"}`)},
				"mod.wasm": {Data: build(t, test.mod)},
			}
			_, err := Mod(context.Background(), dist)
			switch {
			case test.fail == "" && err != nil:
				t.Fatal(err)
			case test.fail != "" && (err == nil || !strings.Contains(err.Error(), test.fail)):
				t.Fatalf("got %v, want an error containing %q", err, test.fail)
			}
		})
	}
}

// TestScript accepts a JavaScript mod and refuses a runtime Modlock does not
// ship or an entry outside the mod.
func TestScript(t *testing.T) {
	for manifest, fail := range map[string]string{
		`{"slug": "js", "version": "1", "runtime": "RUNTIME_QUICKJS", "entry": "main.js"}`:    "",
		`{"slug": "js", "version": "1", "runtime": "RUNTIME_UNKNOWN", "entry": "main.js"}`:    "does not run",
		`{"slug": "js", "version": "1", "runtime": "RUNTIME_QUICKJS", "entry": "../main.js"}`: "not a file",
	} {
		dist := fstest.MapFS{"mod.json": {Data: []byte(manifest)}, "main.js": {Data: []byte("1")}}
		_, err := Mod(context.Background(), dist)
		if fail == "" && err != nil || fail != "" && (err == nil || !strings.Contains(err.Error(), fail)) {
			t.Errorf("%s: got %v, want %q", manifest, err, fail)
		}
	}
}

// build compiles the test mod in testdata/name to WebAssembly.
func build(t *testing.T, name string) []byte {
	t.Helper()
	output := filepath.Join(t.TempDir(), "mod.wasm")
	cmd := exec.Command("go", "build", "-buildmode=c-shared", "-o", output, "./testdata/"+name)
	cmd.Env = append(os.Environ(), "GOOS=wasip1", "GOARCH=wasm")
	if out, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("build %s: %v\n%s", name, err, out)
	}
	module, err := os.ReadFile(output)
	if err != nil {
		t.Fatal(err)
	}
	return module
}
