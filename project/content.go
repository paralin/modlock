package project

import (
	"io/fs"
	"os"
	"path"
	"path/filepath"
	"regexp"
	"slices"
	"strings"
	"testing/fstest"

	"github.com/paralin/modlock/check"
	"github.com/paralin/modlock/internal/fileretry"
	"github.com/paralin/modlock/source2/panorama"
	"github.com/pkg/errors"
)

// ContentDir is a project's directory of client content: the files of the
// mod's content addon, without the addon's name. A file at
// content/<kind>/<path>, such as content/layout/inventory.xml, builds to
// panorama/<kind>/<addon>/<path> in the built mod, and one in an asset kind,
// such as content/models/pawn.vmdl_c, to models/<addon>/pawn.vmdl_c.
const ContentDir = "content"

// addonToken matches a path to one of the addon's own files, which a layout,
// script or style sheet writes with {addon} in place of the addon's name, as
// in file://{resources}/scripts/{addon}/inventory.js.
var addonToken = regexp.MustCompile(`(\{resources\}/[\w.-]+/)\{addon\}/`)

// buildContent compiles the project's content directory into the built mod's,
// replacing the previous one; a failed build leaves it in place. A project
// without content ships none.
func (p *Project) buildContent() error {
	// Read each source to its place in the addon, naming the addon in it.
	addon := check.Addon(p.Manifest)
	sources := fstest.MapFS{}
	root := filepath.Join(p.Dir, ContentDir)
	err := filepath.WalkDir(root, func(file string, entry fs.DirEntry, err error) error {
		switch {
		case file == root && errors.Is(err, fs.ErrNotExist):
			return nil
		case err != nil:
			return err
		case strings.HasPrefix(entry.Name(), "."):
			// Hidden files, such as an editor's, stay out, as the watch skips them.
			if entry.IsDir() {
				return filepath.SkipDir
			}
			return nil
		case !entry.Type().IsRegular():
			return nil
		}
		name, err := filepath.Rel(root, file)
		if err != nil {
			return err
		}
		name = filepath.ToSlash(name)
		kind, rest, ok := strings.Cut(name, "/")
		if !ok {
			return errors.Errorf("%s/%s: put it in a directory for its kind, such as %s/layout/ or %s/models/", ContentDir, name, ContentDir, ContentDir)
		}
		data, err := os.ReadFile(file)
		if err != nil {
			return errors.Wrap(err, "read the content")
		}
		switch path.Ext(rest) {
		case ".xml", ".js", ".css":
			data = addonToken.ReplaceAll(data, []byte("${1}"+addon+"/"))
		}
		place := path.Join(check.ContentDir, kind, addon, rest)
		if slices.Contains(check.AssetDirs, kind) {
			place = path.Join(kind, addon, rest)
		}
		sources[place] = &fstest.MapFile{Data: data}
		return nil
	})
	if err != nil {
		return err
	}
	compiled, err := panorama.CompileTree(addon, sources)
	if err != nil {
		return err
	}

	// Stage the compiled content, then replace the previous content with it.
	staged := filepath.Join(p.Output(), ".content.tmp")
	if err := os.RemoveAll(staged); err != nil {
		return errors.Wrap(err, "write the content")
	}
	defer os.RemoveAll(staged)
	for name, file := range compiled {
		target := filepath.Join(staged, filepath.FromSlash(name))
		if err := os.MkdirAll(filepath.Dir(target), 0o755); err != nil {
			return errors.Wrap(err, "write the content")
		}
		if err := os.WriteFile(target, file.Data, 0o644); err != nil {
			return errors.Wrap(err, "write the content")
		}
	}
	for _, dir := range check.AddonDirs() {
		final := filepath.Join(p.Output(), dir)
		if err := os.RemoveAll(final); err != nil {
			return errors.Wrap(err, "write the content")
		}
		err := fileretry.Rename(filepath.Join(staged, dir), final)
		if err != nil && !errors.Is(err, fs.ErrNotExist) {
			return errors.Wrap(err, "write the content")
		}
	}
	return nil
}
