package main

import (
	"io"
	"os"
	"path/filepath"
	"slices"
	"strings"
	"testing"

	"github.com/paralin/modlock/src/host_app/control"
)

// TestWriteFixtures checks a real-game session hands the host its project's
// fixtures and names who answers each service.
func TestWriteFixtures(t *testing.T) {
	// A project names a needed service with a fixture, an optional one
	// without, and a needed one without.
	dir := t.TempDir()
	build := filepath.Join(dir, "build")
	manifest := `{"slug":"drills","services":[{"name":"example.drills.v1"},` +
		`{"name":"example.ghosts.v1","optional":true},{"name":"example.scores.v1"}]}`
	for _, file := range []string{filepath.Join(dir, "mod.json"), filepath.Join(build, "mod.json")} {
		if err := os.MkdirAll(filepath.Dir(file), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(file, []byte(manifest), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	fixture := filepath.Join(dir, "services", "example.drills.v1.json")
	if err := os.MkdirAll(filepath.Dir(fixture), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(fixture, []byte(`{"read": { "key": "slide" }, "save": true}`), 0o644); err != nil {
		t.Fatal(err)
	}

	// Write the host's file, keeping what the session prints.
	path := filepath.Join(t.TempDir(), "fixtures.pb")
	printed, written := capture(t, func() (string, error) {
		return writeFixtures(&printer{}, []string{build}, path, "")
	})
	if written != path {
		t.Fatalf("wrote %q, want %q", written, path)
	}

	// Each service names its provider, and the missing needed one says how to
	// add it.
	for _, want := range []string{
		"drills: service example.drills.v1: fixture " + fixture,
		"drills: service example.ghosts.v1: nobody (optional)",
		"drills: service example.scores.v1: nobody; the mod needs it, so add " +
			filepath.Join("services", "example.scores.v1.json"),
	} {
		if !strings.Contains(printed, want) {
			t.Errorf("output lacks %q:\n%s", want, printed)
		}
	}

	// The file holds each method's compact reply, in method order.
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	fixtures := &control.Fixtures{}
	if err := fixtures.UnmarshalVT(data); err != nil {
		t.Fatal(err)
	}
	var got []string
	for _, each := range fixtures.GetFixtures() {
		got = append(got, each.GetMod()+" "+each.GetService()+" "+each.GetMethod()+" "+
			string(each.GetPayload())+" "+each.GetSource())
	}
	want := []string{
		"drills example.drills.v1 read {\"key\":\"slide\"} " + fixture,
		"drills example.drills.v1 save true " + fixture,
	}
	if strings.Join(got, "\n") != strings.Join(want, "\n") {
		t.Fatalf("fixtures:\n%s\nwant:\n%s", strings.Join(got, "\n"), strings.Join(want, "\n"))
	}

	// A built mod outside a project, with no fixture, writes no file.
	none, err := writeFixtures(&printer{brief: true}, []string{dir}, filepath.Join(t.TempDir(), "none.pb"), "")
	if err != nil || none != "" {
		t.Fatalf("a mod without fixtures wrote %q, %v", none, err)
	}

	// A forward origin forwards every service the mod names, fixture or not.
	forwarded := filepath.Join(t.TempDir(), "forwarded.pb")
	printed, _ = capture(t, func() (string, error) {
		return writeFixtures(&printer{}, []string{build}, forwarded, "http://127.0.0.1:9")
	})
	data, err = os.ReadFile(forwarded)
	if err != nil {
		t.Fatal(err)
	}
	fixtures = &control.Fixtures{}
	if err := fixtures.UnmarshalVT(data); err != nil {
		t.Fatal(err)
	}
	got = nil
	for _, each := range fixtures.GetForwards() {
		got = append(got, each.GetMod()+" "+each.GetService()+" "+each.GetTarget())
		if !strings.Contains(printed, "drills: service "+each.GetService()+": forwarded to http://127.0.0.1:9") {
			t.Errorf("output lacks the forward of %s:\n%s", each.GetService(), printed)
		}
	}
	if len(fixtures.GetFixtures()) != 0 || !slices.Equal(got, []string{
		"drills example.drills.v1 http://127.0.0.1:9",
		"drills example.ghosts.v1 http://127.0.0.1:9",
		"drills example.scores.v1 http://127.0.0.1:9",
	}) {
		t.Errorf("forwarded %q with %d fixtures", got, len(fixtures.GetFixtures()))
	}
}

// TestForwarderNeedsASafeOrigin forwards nothing without the flag and refuses
// an origin that would send the session in the clear.
func TestForwarderNeedsASafeOrigin(t *testing.T) {
	if answer, err := forwarder(""); answer != nil || err != nil {
		t.Errorf("no origin: %v, %v", answer != nil, err)
	}
	if _, err := forwarder("http://example.com"); err == nil {
		t.Error("a plain http origin was taken")
	}
}

// capture runs fn with standard output kept, returning what it printed and
// fn's path, and failing the test on fn's error.
func capture(t *testing.T, fn func() (string, error)) (string, string) {
	t.Helper()
	reader, writer, err := os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	stdout := os.Stdout
	os.Stdout = writer
	path, fnErr := fn()
	os.Stdout = stdout
	_ = writer.Close()
	printed, _ := io.ReadAll(reader)
	if fnErr != nil {
		t.Fatal(fnErr)
	}
	return string(printed), path
}
