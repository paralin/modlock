package project

import (
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/paralin/modlock/src/wasm/manifest"
)

// TestManifestRoundTrip checks that Open reads what WriteManifest writes,
// including enums by name, quoted text and nested messages, and that the
// built manifest keeps the fields Modlock does not read.
func TestManifestRoundTrip(t *testing.T) {
	// Write a source manifest.
	dir := t.TempDir()
	written := &manifest.Manifest{
		Slug:     "parry-ball",
		Name:     `Parry "Ball"`,
		Version:  "0.1.0",
		Language: manifest.Manifest_LANGUAGE_GO,
		Movement: &manifest.Movement{Model: manifest.Movement_MODEL_QUAKEWORLD, Scale: 1.25},
	}
	if err := WriteManifest(filepath.Join(dir, ManifestFile), written); err != nil {
		t.Fatal(err)
	}

	// Read it back as a project.
	opened, err := Open(dir)
	if err != nil {
		t.Fatal(err)
	}
	if !opened.Manifest.EqualVT(written) {
		t.Fatalf("read %v, wrote %v", opened.Manifest, written)
	}
	data, err := os.ReadFile(filepath.Join(dir, ManifestFile))
	if err != nil {
		t.Fatal(err)
	}
	want := "{\n  \"slug\": \"parry-ball\",\n  \"name\": \"Parry \\\"Ball\\\"\",\n  \"version\": \"0.1.0\",\n  \"language\": \"LANGUAGE_GO\",\n  \"movement\": {\n    \"model\": \"MODEL_QUAKEWORLD\",\n    \"scale\": 1.25\n  }\n}\n"
	if string(data) != want {
		t.Fatalf("wrote %q, want %q", data, want)
	}

	// A store's field in the project's mod.json passes into the built one,
	// which names the runtime and entry in place of the language.
	source := strings.Replace(want, "{\n", "{\n  \"summary\": \"A ring duel.\",\n", 1)
	if err := os.WriteFile(filepath.Join(dir, ManifestFile), []byte(source), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(opened.Output(), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := opened.writeBuiltManifest(manifest.Manifest_RUNTIME_WASM, Entry); err != nil {
		t.Fatal(err)
	}
	built, err := os.ReadFile(filepath.Join(opened.Output(), ManifestFile))
	if err != nil {
		t.Fatal(err)
	}
	for _, line := range []string{`"summary": "A ring duel."`, `"runtime": "RUNTIME_WASM"`, `"entry": "mod.wasm"`} {
		if !strings.Contains(string(built), line) {
			t.Fatalf("built %s, want %s in it", built, line)
		}
	}
	if strings.Contains(string(built), "language") {
		t.Fatalf("built %s names the language", built)
	}
}
