package project

import (
	"context"
	"io"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/paralin/modlock/js"
	manifestpb "github.com/paralin/modlock/src/wasm/manifest"
	"github.com/pkg/errors"
)

// TestBuildRelease checks that a release bundles a script mod minified with
// Rolldown and a dev build bundles it readable with esbuild, both keeping the
// mod's code and turning a style sheet import into the call that adds it.
func TestBuildRelease(t *testing.T) {
	if testing.Short() {
		t.Skip("downloads Node and Rolldown")
	}
	ctx := context.Background()
	p, err := New(ctx, filepath.Join(t.TempDir(), "probe"), "", manifestpb.Manifest_LANGUAGE_TYPESCRIPT, "", io.Discard)
	if err != nil {
		t.Fatal(err)
	}

	// Import a sheet from the content styles directory.
	styles := filepath.Join(p.Dir, ContentDir, "styles", "hud")
	if err := os.MkdirAll(styles, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(styles, "board.css"), []byte(".Board { color: red; }\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	main := filepath.Join(p.Dir, p.scriptEntry())
	source, err := os.ReadFile(main)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(main, append([]byte("import './content/styles/hud/board.css'\n"), source...), 0o644); err != nil {
		t.Fatal(err)
	}

	built := map[Mode]string{}
	for _, mode := range []Mode{Dev, Release} {
		if err := p.Build(ctx, io.Discard, mode); err != nil {
			t.Fatalf("build mode %d: %v", mode, err)
		}
		script, err := os.ReadFile(filepath.Join(p.Output(), ScriptEntry))
		if err != nil {
			t.Fatal(err)
		}
		built[mode] = string(script)
	}
	for mode, script := range built {
		if !strings.Contains(script, "Hello from Probe!") {
			t.Errorf("build mode %d lost the greeting", mode)
		}
		if !strings.Contains(script, "hud/board.css") {
			t.Errorf("build mode %d does not add the sheet", mode)
		}
	}
	layouts, _ := filepath.Glob(filepath.Join(p.Output(), "panorama", "layout", "*", "hud", "board.css.vxml_c"))
	if len(layouts) != 1 {
		t.Errorf("the build holds sheet layouts %v", layouts)
	}

	// A sheet outside the styles directory fails each build at its import.
	if err := os.WriteFile(filepath.Join(p.Dir, "board.css"), nil, 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(main, append([]byte("import './board.css'\n"), source...), 0o644); err != nil {
		t.Fatal(err)
	}
	for _, mode := range []Mode{Dev, Release} {
		err := p.Build(ctx, io.Discard, mode)
		var check *CheckError
		if !errors.As(err, &check) || len(check.Diagnostics) == 0 ||
			!strings.Contains(check.Diagnostics[0].Message, "content/styles/") ||
			filepath.Base(check.Diagnostics[0].File) != p.scriptEntry() {
			t.Errorf("build mode %d of a sheet outside the styles directory: %v %v", mode, err, check.Diagnostics)
		}
	}
	if len(built[Release]) >= len(built[Dev])/2 {
		t.Errorf("release is %d bytes, dev %d: not minified", len(built[Release]), len(built[Dev]))
	}
	if strings.Count(built[Release], "\n") > 2 {
		t.Errorf("release has %d lines, want one", strings.Count(built[Release], "\n")+1)
	}
}

// TestBundleRolldownError checks that a mod Rolldown cannot bundle fails with
// a located diagnostic.
func TestBundleRolldownError(t *testing.T) {
	if testing.Short() {
		t.Skip("downloads Node and Rolldown")
	}
	ctx := context.Background()
	dir, err := filepath.EvalSymlinks(t.TempDir())
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "main.ts"), []byte("import { x } from './missing.js'\nconsole.log(x)\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	library, err := js.Library(ctx)
	if err != nil {
		t.Fatal(err)
	}
	p := &Project{Dir: dir, Manifest: &manifestpb.Manifest{}}
	_, err = p.bundleRolldown(ctx, library, io.Discard)
	var check *CheckError
	if !errors.As(err, &check) {
		t.Fatalf("got %v, want a CheckError", err)
	}
	if len(check.Diagnostics) == 0 || !strings.Contains(check.Diagnostics[0].Message, "missing") {
		t.Fatalf("diagnostics %v do not name the missing import", check.Diagnostics)
	}
	if got := check.Diagnostics[0].File; got != filepath.Join(dir, "main.ts") {
		t.Errorf("diagnostic file %q, want main.ts", got)
	}
}
