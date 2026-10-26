package project

import (
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/paralin/modlock/check"
	"github.com/paralin/modlock/source2/panorama"
	manifestpb "github.com/paralin/modlock/src/wasm/manifest"
)

// TestContent builds a project's content, compiled models included, into its
// addon's directories with the addon named in its paths, passes the publishing check, tells content
// that runs code from content that only draws, refuses a file outside a
// kind's directory, fingerprints the built mod apart from notes and logs, and
// removes the content once the project has none, and ships the map.
func TestContent(t *testing.T) {
	// Lay out a project with a layout that loads its script, and an editor's
	// hidden file that must stay out.
	dir := t.TempDir()
	write := func(name, content string) {
		t.Helper()
		file := filepath.Join(dir, filepath.FromSlash(name))
		if err := os.MkdirAll(filepath.Dir(file), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(file, []byte(content), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	write("content/layout/panel.xml", `<root><scripts><include src="file://{resources}/scripts/{addon}/panel.js" /></scripts><Panel /></root>`)
	write("content/scripts/panel.js", "$.Msg('{addon} stays as written outside a path');")
	write("content/layout/.panel.xml.swp", "swap")
	write("content/models/pawn.vmdl_c", "compiled")
	manifest := &manifestpb.Manifest{Slug: "parry", Version: "1.2.0", Language: manifestpb.Manifest_LANGUAGE_TYPESCRIPT}
	p := &Project{Dir: dir, Manifest: manifest}
	addon := check.Addon(manifest)

	// Build the content and read it back.
	if err := p.buildContent(); err != nil {
		t.Fatal(err)
	}
	built := filepath.Join(p.Output(), check.ContentDir)
	layout, err := os.ReadFile(filepath.Join(built, "layout", addon, "panel.vxml_c"))
	if err != nil {
		t.Fatal(err)
	}
	if source, err := panorama.Decompile(layout); err != nil || !strings.Contains(source, "panorama/scripts/parry-1.2.0/panel.vjs_c") {
		t.Errorf("layout %q, %v; want the script under the addon's name", source, err)
	}
	if _, err := os.Stat(filepath.Join(built, "scripts", addon, "panel.vjs_c")); err != nil {
		t.Error(err)
	}
	if model, err := os.ReadFile(filepath.Join(p.Output(), "models", addon, "pawn.vmdl_c")); err != nil || string(model) != "compiled" {
		t.Errorf("model %q, %v; want the compiled resource as written under models/%s", model, err, addon)
	}
	if _, err := os.Stat(filepath.Join(built, "layout", addon, ".panel.xml.swp")); err == nil {
		t.Error("a hidden file shipped")
	}

	// The built content passes the publishing check, and a file outside the
	// addon's directories does not.
	if err := check.Content(os.DirFS(p.Output()), addon); err != nil {
		t.Error(err)
	}
	if err := check.Content(os.DirFS(p.Output()), "parry-1.1.0"); err == nil {
		t.Error("content under another release's name passed")
	}

	// The layout and its script run code in players' games; a layout that
	// handles an event does too, and one that only draws does not.
	scripted := func(want string) {
		t.Helper()
		if err := p.buildContent(); err != nil {
			t.Fatal(err)
		}
		if got, err := check.Scripted(os.DirFS(p.Output())); err != nil || got != want {
			t.Errorf("scripted %q, %v; want %q", got, err, want)
		}
	}
	scripted("panorama/layout/parry-1.2.0/panel.vxml_c")
	if err := os.Remove(filepath.Join(dir, ContentDir, "scripts", "panel.js")); err != nil {
		t.Fatal(err)
	}
	write("content/layout/panel.xml", `<root><Panel><Button onactivate="$.Msg('hi')" /></Panel></root>`)
	scripted("panorama/layout/parry-1.2.0/panel.vxml_c")
	write("content/layout/panel.xml", `<root><styles><include src="s2r://panorama/styles/hud.vcss_c" /></styles><Panel><Label text="Ready" /></Panel></root>`)
	scripted("")

	// A note in the project and the server's log leave the built mod's
	// fingerprint alone, so a watch does not reload the game for them; a
	// content change moves it.
	fingerprint := func() [32]byte {
		t.Helper()
		if err := p.buildContent(); err != nil {
			t.Fatal(err)
		}
		sum, err := p.Fingerprint()
		if err != nil {
			t.Fatal(err)
		}
		return sum
	}
	before := fingerprint()
	write("guides/evidence.tsv", "hash\tfile")
	write(BuildDir+"/server.log", "line")
	if fingerprint() != before {
		t.Error("a note or a log changed the fingerprint")
	}
	write("content/layout/panel.xml", `<root><Panel><Label text="Go" /></Panel></root>`)
	if fingerprint() == before {
		t.Error("a content change kept the fingerprint")
	}

	// A file outside a kind's directory fails the build and keeps the last
	// content.
	write("content/notes.txt", "notes")
	if err := p.buildContent(); err == nil || !strings.Contains(err.Error(), "content/notes.txt") {
		t.Errorf("got %v, want an error naming content/notes.txt", err)
	}
	if _, err := os.Stat(filepath.Join(built, "layout", addon, "panel.vxml_c")); err != nil {
		t.Error(err)
	}

	// Without content, the build ships none.
	if err := os.RemoveAll(filepath.Join(dir, ContentDir)); err != nil {
		t.Fatal(err)
	}
	if err := p.buildContent(); err != nil {
		t.Fatal(err)
	}
	for _, dir := range check.AddonDirs() {
		if _, err := os.Stat(filepath.Join(p.Output(), dir)); !os.IsNotExist(err) {
			t.Errorf("%s remains: %v", dir, err)
		}
	}

	// The build ships the project's map, keeps the copy while the map is
	// unchanged, and replaces it when the map changes.
	manifest.Map = "arena"
	shipped := func(want string) {
		t.Helper()
		if err := p.shipMap(); err != nil {
			t.Fatal(err)
		}
		if got, err := os.ReadFile(filepath.Join(p.Output(), check.MapsDir, "arena.vpk")); err != nil || string(got) != want {
			t.Errorf("map %q, %v; want %q", got, err, want)
		}
	}
	write("maps/arena.vpk", "first")
	shipped("first")
	shipped("first")
	write("maps/arena.vpk", "second map")
	shipped("second map")
}
