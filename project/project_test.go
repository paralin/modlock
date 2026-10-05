package project

import (
	"os"
	"path/filepath"
	"testing"

	"github.com/paralin/modlock/proto/modlock/wasm"
)

// TestManifestRoundTrip checks that Open reads what WriteManifest writes,
// including enums by name, quoted text and nested messages.
func TestManifestRoundTrip(t *testing.T) {
	// Write a source manifest.
	dir := t.TempDir()
	written := &wasm.Manifest{
		Slug:     "parry-ball",
		Name:     `Parry "Ball"`,
		Version:  "0.1.0",
		Language: wasm.Manifest_LANGUAGE_GO,
		Movement: &wasm.Movement{Model: wasm.Movement_MODEL_QUAKEWORLD, Scale: 1.25},
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
}
