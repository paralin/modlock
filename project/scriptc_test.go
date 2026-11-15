package project

import (
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/paralin/modlock/js"
)

// TestScriptcConfigResolvesProtobuf checks that the scriptc configuration
// resolves the protobuf runtime's modules to the shipped source, the
// well-known types under google/protobuf as well as message and enum.
func TestScriptcConfigResolvesProtobuf(t *testing.T) {
	// Ship the runtime's source files as the built library lays them out.
	library := t.TempDir()
	protobuf := filepath.Join(library, js.Scriptc, js.ScriptcProtobuf)
	shipped := []string{
		"index.ts",
		"message.ts",
		"enum.ts",
		"google/protobuf/timestamp.pb.ts",
		"google/protobuf/struct.pb.ts",
		"google/protobuf/wrappers.pb.ts",
	}
	for _, name := range shipped {
		path := filepath.Join(protobuf, filepath.FromSlash(name))
		if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(path, nil, 0o644); err != nil {
			t.Fatal(err)
		}
	}

	// Write the configuration for a project with no tsconfig of its own.
	dir := t.TempDir()
	p := &Project{Dir: t.TempDir()}
	if err := p.writeScriptcConfig(dir, library, nil); err != nil {
		t.Fatal(err)
	}
	data, err := os.ReadFile(filepath.Join(dir, "tsconfig.json"))
	if err != nil {
		t.Fatal(err)
	}
	var config scriptcConfig
	if err := json.Unmarshal(data, &config); err != nil {
		t.Fatal(err)
	}

	// Each import the generated messages make lands on a shipped file.
	for specifier, want := range map[string]string{
		"@aptre/protobuf-es-lite":                           "index.ts",
		"@aptre/protobuf-es-lite/message":                   "message.ts",
		"@aptre/protobuf-es-lite/enum":                      "enum.ts",
		"@aptre/protobuf-es-lite/google/protobuf/timestamp": "google/protobuf/timestamp.pb.ts",
		"@aptre/protobuf-es-lite/google/protobuf/struct":    "google/protobuf/struct.pb.ts",
		"@aptre/protobuf-es-lite/google/protobuf/wrappers":  "google/protobuf/wrappers.pb.ts",
	} {
		got := resolvePath(config.CompilerOptions.Paths, specifier)
		if got != filepath.Join(protobuf, filepath.FromSlash(want)) {
			t.Errorf("%s resolves to %q, want %s", specifier, got, want)
			continue
		}
		if _, err := os.Stat(got); err != nil {
			t.Errorf("%s resolves to a missing file: %v", specifier, err)
		}
	}
}

// resolvePath resolves specifier through paths as TypeScript does: an exact
// key first, then the wildcard key with the longest prefix, taking the first
// target with the wildcard's match substituted.
func resolvePath(paths map[string][]string, specifier string) string {
	// Take an exact key.
	if targets, ok := paths[specifier]; ok && len(targets) != 0 {
		return targets[0]
	}

	// Otherwise take the matching wildcard key with the longest prefix.
	best, bestPrefix, match := "", "", ""
	for key := range paths {
		prefix, suffix, ok := strings.Cut(key, "*")
		if !ok || !strings.HasPrefix(specifier, prefix) || !strings.HasSuffix(specifier, suffix) {
			continue
		}
		if len(specifier) < len(prefix)+len(suffix) || best != "" && len(prefix) <= len(bestPrefix) {
			continue
		}
		best, bestPrefix, match = key, prefix, specifier[len(prefix):len(specifier)-len(suffix)]
	}
	if best == "" || len(paths[best]) == 0 {
		return ""
	}
	return strings.Replace(paths[best][0], "*", match, 1)
}
