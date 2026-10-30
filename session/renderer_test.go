package session

import (
	"bytes"
	"maps"
	"os"
	"path/filepath"
	"testing"
	"testing/fstest"

	"github.com/paralin/modlock/devmount"
	"github.com/paralin/modlock/source2/vpk"
)

// TestMountRendererCarriesMaps mounts a mod that ships its own map: the map's
// archive lands beside the package where the game finds it, its assets go
// into the package, a rebuild adds only its changed files loose beside it, and
// the next session without the map removes it.
func TestMountRendererCarriesMaps(t *testing.T) {
	// A game with a stock gameinfo.gi, and a built mod with a map.
	gameDir := t.TempDir()
	info := filepath.Join(gameDir, "game", "citadel", "gameinfo.gi")
	stock := "\"GameInfo\"\n{\n\tFileSystem\n\t{\n\t\tSearchPaths\n\t\t{\n\t\t\tGame\t\t\t\tcitadel\n\t\t}\n\t}\n}\n"
	mod := t.TempDir()
	arena := []byte("map archive")
	for name, data := range map[string][]byte{
		info:                                    []byte(stock),
		filepath.Join(mod, "maps", "arena.vpk"): arena,
		filepath.Join(mod, "maps", "arena.bsp"): []byte("quake collision"),
		filepath.Join(mod, "materials", "arena", "turf.vmat_c"): []byte("material"),
	} {
		if err := os.MkdirAll(filepath.Dir(name), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(name, data, 0o644); err != nil {
			t.Fatal(err)
		}
	}

	// The session mounts the map beside the package and the assets in it.
	content, err := ReadContent([]string{mod})
	if err != nil {
		t.Fatal(err)
	}
	mounted, err := mountRenderer(gameDir, nil, "", content)
	if err != nil {
		t.Fatal(err)
	}
	found, _ := filepath.Glob(filepath.Join(devmount.Path(gameDir), "*", "maps", "arena.vpk"))
	if len(found) != 1 {
		t.Fatalf("map archives beside the package: %v", found)
	}
	mapped := found[0]
	if got, err := os.ReadFile(mapped); err != nil || !bytes.Equal(got, arena) {
		t.Fatalf("map archive beside the package: %q, %v", got, err)
	}
	pak, err := vpk.OpenArchive(filepath.Join(filepath.Dir(filepath.Dir(mapped)), "pak01_dir.vpk"))
	if err != nil {
		t.Fatal(err)
	}
	if _, err := pak.ReadFile("materials/arena/turf.vmat_c"); err != nil {
		t.Fatalf("material in the package: %v", err)
	}
	for _, name := range []string{"maps/arena.vpk", "maps/arena.bsp"} {
		if _, err := pak.ReadFile(name); err == nil {
			t.Fatalf("%s is inside the package", name)
		}
	}

	// A rebuild adds its new files loose beside the archive, and only those.
	rebuilt := maps.Clone(content)
	rebuilt["panorama/styles/arena-1234abcd/hud.vcss_c"] = &fstest.MapFile{Data: []byte("sheet")}
	if err := mounted.Add(rebuilt); err != nil {
		t.Fatal(err)
	}
	if got, err := os.ReadFile(filepath.Join(mounted.dir, "panorama", "styles", "arena-1234abcd", "hud.vcss_c")); err != nil || string(got) != "sheet" {
		t.Fatalf("rebuilt sheet beside the package: %q, %v", got, err)
	}
	if _, err := os.Stat(filepath.Join(mounted.dir, "materials")); !os.IsNotExist(err) {
		t.Fatalf("an unchanged file was written loose: %v", err)
	}
	if err := mounted.Close(); err != nil {
		t.Fatal(err)
	}

	// A session whose mods ship no map removes the one left behind.
	if err := os.RemoveAll(filepath.Join(mod, "maps")); err != nil {
		t.Fatal(err)
	}
	if content, err = ReadContent([]string{mod}); err != nil {
		t.Fatal(err)
	}
	if mounted, err = mountRenderer(gameDir, nil, "", content); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(mapped); !os.IsNotExist(err) {
		t.Fatalf("a previous session's map stayed: %v", err)
	}
	if err := mounted.Close(); err != nil {
		t.Fatal(err)
	}
}
