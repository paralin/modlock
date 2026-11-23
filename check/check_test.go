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
	// Build the mod for WebAssembly and read the module.
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

// TestMap accepts no map, a stock map, or an hl_ map the build ships, and
// refuses a map only its author's game has, such as one compiled into
// citadel/maps under another name, or an hl_ map the build left out.
func TestMap(t *testing.T) {
	shipped := fstest.MapFS{"maps/hl_arena_pit.vpk": {Data: []byte("vpk")}}
	for _, test := range []struct {
		name string
		dist fstest.MapFS
		fail string
	}{
		{"", nil, ""},
		{"dl_midtown", nil, ""},
		{"hl_arena_pit", shipped, ""},
		{"hl_arena_pit", nil, "has no maps/hl_arena_pit.vpk"},
		{"arena_pit_v1", nil, "hl_<name>"},
		{"dl_hideout", nil, "hl_<name>"},
	} {
		err := Map(test.dist, test.name)
		switch {
		case test.fail == "" && err != nil:
			t.Fatal(test.name, err)
		case test.fail != "" && (err == nil || !strings.Contains(err.Error(), test.fail)):
			t.Fatalf("%s: got %v, want an error containing %q", test.name, err, test.fail)
		}
	}
}

// TestContentName accepts files inside an addon's directory and a mod's map,
// and refuses every name that could land outside its package on some
// platform or replace a file of the game's: a climb, a backslash or a drive,
// an absolute path, a stock path, and a stock map.
func TestContentName(t *testing.T) {
	for name, addon := range map[string]string{
		"panorama/layout/arena-0123abcd/hud.vxml_c": "arena-0123abcd",
		"models/arena-0123abcd/pawn.vmdl_c":         "arena-0123abcd",
		"materials/arena-0.1.0/a/b.vmat_c":          "arena-0.1.0",
		"maps/hl_arena.vpk":                         "",
	} {
		if got, err := ContentName(name); err != nil || got != addon {
			t.Errorf("%s: got %q, %v, want %q", name, got, err, addon)
		}
	}
	for _, name := range []string{
		"maps/../pak01_dir.vpk",
		`maps/hl_x\..\..\pak01_dir.vpk`,
		`maps/..\..\pak01_dir.vpk`,
		`panorama/layout/arena-0123abcd/..\..\hud\hud.vxml_c`,
		"panorama/layout/arena-0123abcd/C:x.vxml_c",
		"/panorama/layout/arena-0123abcd/hud.vxml_c",
		"C:/Windows/x.vpk",
		"panorama/layout/hud/hud.vxml_c",
		"panorama/layout/arena-0123abcd",
		"models/heroes/abrams.vmdl_c",
		"maps/dl_midtown.vpk",
		"maps/hl_arena/x.vpk",
		"maps/hl_arena.bsp",
		"pak01_dir.vpk",
		"scripts/vscripts/x.lua",
		"panorama/layout/arena-0123abcd/a\x00.vxml_c",
	} {
		if _, err := ContentName(name); err == nil {
			t.Errorf("%q passed", name)
		}
	}
}
