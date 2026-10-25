package session

import (
	"bytes"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"io/fs"
	"maps"
	"os"
	"path"
	"path/filepath"
	"slices"
	"strings"
	"testing/fstest"

	"github.com/paralin/modlock/check"
	"github.com/paralin/modlock/devmount"
	"github.com/paralin/modlock/panorama"
	s2panorama "github.com/paralin/modlock/source2/panorama"
	"github.com/paralin/modlock/source2/vpk"
	"github.com/pkg/errors"
)

const (
	// newsLayout is the stock layout the package shadows. The game loads it
	// at startup inside the hideout HUD and keeps it for the whole process,
	// so a script it includes can draw over the HUD in every match.
	newsLayout = "panorama/layout/latest_news.xml"
	// bootScript loads the renderer's layout from the news layout.
	bootScript = "panorama/scripts/modlock/boot.js"
)

// bootSource creates a pass-through panel in the news layout and loads the
// renderer into it.
const bootSource = `(function () {
  var host = $.CreatePanel('Panel', $.GetContextPanel(), 'ModlockUiHost')
  host.hittest = false
  host.BLoadLayout('file://{resources}/layout/modlock/ui.xml', false, false)
})()
`

// Renderer is the built scripts that draw the mods' interfaces in the game.
type Renderer struct {
	// Script draws each mod's tree; it installs as scripts/modlock/ui.js.
	Script []byte
	// Layout is the library a mod's layout includes to read its data and
	// routes; it installs as scripts/modlock/layout.js.
	Layout []byte
}

// mountRenderer mounts a package in the Deadlock installation at gameDir that
// draws the mods' interfaces in the player's game and carries each mod's
// content addon, and returns the call that unmounts it. renderer is the built
// renderer, or nil to leave the interfaces out, and content is the mods'
// content addons. A map archive in content, maps/<map>.vpk, is written beside
// the package instead of into it, since the game opens each map as its own
// archive on the search path, as it does the stock maps.
// The renderer shadows the stock news layout, read from the installed game so
// it follows game updates, with one script added. The package outranks every
// other mounted package while the session runs, and the server and the game
// load it only when they start. The session holds the mount live until it
// unmounts, so another program clears the block only after a session that did
// not end cleanly, and the next session replaces such a block too. Each
// package goes in a directory named for its content, so a game still running
// from an earlier session keeps its package while the next session mounts a
// changed one.
func mountRenderer(gameDir string, renderer *Renderer, content fstest.MapFS) (func() error, error) {
	// Build the package from the renderer and the mods' content.
	citadel := filepath.Join(gameDir, "game", "citadel")
	files := fstest.MapFS{}
	if renderer != nil {
		compiled, err := rendererFiles(citadel, renderer)
		if err != nil {
			return nil, err
		}
		maps.Copy(files, compiled)
	}
	maps.Copy(files, content)
	archives := fstest.MapFS{}
	for name, file := range files {
		if isMapArchive(name) {
			archives[name] = file
			delete(files, name)
		}
	}
	if len(files) == 0 && len(archives) == 0 {
		return func() error { return nil }, nil
	}

	// Name the package for its content, so a changed one goes beside the one
	// a running game holds open instead of over it.
	var archive bytes.Buffer
	if err := vpk.Write(&archive, files); err != nil {
		return nil, err
	}
	archives["pak01_dir.vpk"] = &fstest.MapFile{Data: archive.Bytes()}
	key := packageKey(archives)

	// Prune the packages no game holds, hold the mount live, write the
	// package, then mount it.
	if err := devmount.Prune(gameDir, key); err != nil {
		return nil, err
	}
	held, err := devmount.Hold(gameDir)
	if err != nil {
		return nil, err
	}
	if err := writePackage(devmount.PackagePath(gameDir, key), archives); err != nil {
		_ = held.Close()
		return nil, err
	}
	if err := devmount.Mount(gameDir, key); err != nil {
		_ = held.Close()
		return nil, err
	}
	return func() error {
		defer held.Close()
		return devmount.Unmount(gameDir)
	}, nil
}

// packageKey names a package for its files: the first 16 hex digits of a
// SHA-256 over each file's name and content in name order.
func packageKey(files fstest.MapFS) string {
	hash := sha256.New()
	for _, name := range slices.Sorted(maps.Keys(files)) {
		data := files[name].Data
		_ = binary.Write(hash, binary.LittleEndian, uint64(len(name)))
		hash.Write([]byte(name))
		_ = binary.Write(hash, binary.LittleEndian, uint64(len(data)))
		hash.Write(data)
	}
	return hex.EncodeToString(hash.Sum(nil))[:16]
}

// writePackage writes files into dir, skipping each one already there with
// the same content, as it is when an earlier session wrote the same package.
func writePackage(dir string, files fstest.MapFS) error {
	for name, file := range files {
		target := filepath.Join(dir, filepath.FromSlash(name))
		if current, err := os.ReadFile(target); err == nil && bytes.Equal(current, file.Data) {
			continue
		}
		if err := os.MkdirAll(filepath.Dir(target), 0o755); err != nil {
			return errors.Wrap(err, "write the mods' package")
		}
		if err := os.WriteFile(target, file.Data, 0o644); err != nil {
			return errors.Wrap(err, "write the mods' package")
		}
	}
	return nil
}

// isMapArchive reports whether name in a content addon is a map's archive.
func isMapArchive(name string) bool {
	return path.Dir(name) == check.MapsDir && path.Ext(name) == ".vpk"
}

// rendererFiles compiles the renderer's files: the installed news layout
// with the boot script added, the boot script, and the renderer's layout and
// scripts.
func rendererFiles(citadel string, renderer *Renderer) (fstest.MapFS, error) {
	stock, err := vpk.OpenArchive(filepath.Join(citadel, "pak01_dir.vpk"))
	if err != nil {
		return nil, err
	}
	compiled, err := stock.ReadFile(strings.TrimSuffix(newsLayout, ".xml") + ".vxml_c")
	if err != nil {
		return nil, errors.Wrap(err, "read the game's news layout")
	}
	news, err := s2panorama.Decompile(compiled)
	if err != nil {
		return nil, errors.Wrap(err, "read the game's news layout")
	}
	news, err = withScript(news, "file://{resources}/scripts/modlock/boot.js")
	if err != nil {
		return nil, err
	}
	return s2panorama.CompileTree("modlock_dev", fstest.MapFS{
		newsLayout:                           {Data: []byte(news)},
		bootScript:                           {Data: []byte(bootSource)},
		"panorama/layout/modlock/ui.xml":     {Data: panorama.Layout},
		"panorama/scripts/modlock/ui.js":     {Data: renderer.Script},
		"panorama/scripts/modlock/layout.js": {Data: renderer.Layout},
	})
}

// ReadContent returns the content addons of the mods built at mods: their
// compiled Panorama content and assets, each already under its addon's name,
// and the archive of each map a mod ships, as maps/<map>.vpk.
func ReadContent(mods []string) (fstest.MapFS, error) {
	files := fstest.MapFS{}
	for _, mod := range mods {
		if err := addContent(files, mod); err != nil {
			return nil, err
		}
	}
	return files, nil
}

// addContent adds the content addon of the mod built at dir to files.
func addContent(files fstest.MapFS, dir string) error {
	built := os.DirFS(dir)
	for _, kind := range check.AddonDirs() {
		err := fs.WalkDir(built, kind, func(name string, entry fs.DirEntry, err error) error {
			switch {
			case name == kind && errors.Is(err, fs.ErrNotExist):
				return nil
			case err != nil || !entry.Type().IsRegular():
				return err
			}
			data, err := fs.ReadFile(built, name)
			if err != nil {
				return err
			}
			files[name] = &fstest.MapFile{Data: data, Mode: 0o644}
			return nil
		})
		if err != nil {
			return errors.Wrap(err, "read the mod's content")
		}
	}
	archives, _ := fs.Glob(built, path.Join(check.MapsDir, "*.vpk"))
	for _, name := range archives {
		data, err := fs.ReadFile(built, name)
		if err != nil {
			return errors.Wrap(err, "read the mod's map")
		}
		files[name] = &fstest.MapFile{Data: data, Mode: 0o644}
	}
	return nil
}

// withScript adds an include of the script at src to a layout's scripts.
func withScript(layout, src string) (string, error) {
	include := `<include src="` + src + `" />`
	switch {
	case strings.Contains(layout, "<scripts>"):
		return strings.Replace(layout, "<scripts>", "<scripts>\n"+include, 1), nil
	case strings.Contains(layout, "<root>"):
		return strings.Replace(layout, "<root>", "<root>\n<scripts>"+include+"</scripts>", 1), nil
	default:
		return "", errors.New("the game's news layout has no root to add the renderer to")
	}
}
