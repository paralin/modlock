package session

import (
	"bytes"
	"io/fs"
	"maps"
	"os"
	"path/filepath"
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
// renderer, or nil to leave the interfaces out, and mods are the built mods.
// The renderer shadows the stock news layout, read from the installed game so
// it follows game updates, with one script added. The package outranks every
// other mounted package while the session runs, and the server and the game
// load it only when they start. The session holds the mount live until it
// unmounts, so another program clears the block only after a session that did
// not end cleanly, and the next session replaces such a block too.
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
	if len(files) == 0 {
		return func() error { return nil }, nil
	}

	// Hold the mount live, write the package, then mount it.
	var archive bytes.Buffer
	if err := vpk.Write(&archive, files); err != nil {
		return nil, err
	}
	held, err := devmount.Hold(gameDir)
	if err != nil {
		return nil, err
	}
	unmount := func() error {
		defer held.Close()
		return devmount.Unmount(gameDir)
	}
	// A running game holds the package open on Windows, so leave an
	// unchanged package as it is.
	pak := filepath.Join(devmount.Path(gameDir), "pak01_dir.vpk")
	if current, err := os.ReadFile(pak); err != nil || !bytes.Equal(current, archive.Bytes()) {
		if err := os.WriteFile(pak, archive.Bytes(), 0o644); err != nil {
			_ = held.Close()
			return nil, err
		}
	}
	if err := devmount.Mount(gameDir); err != nil {
		_ = held.Close()
		return nil, err
	}
	return unmount, nil
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
// compiled Panorama content and assets, each already under its addon's name.
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
