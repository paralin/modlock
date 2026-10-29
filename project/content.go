package project

import (
	"crypto/sha256"
	"fmt"
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
// in file://{resources}/scripts/{addon}/inventory.js or
// file://{images}/{addon}/mark.png.
var addonToken = regexp.MustCompile(`(\{resources\}/[\w.-]+/|\{images\}/)\{addon\}/`)

// buildContent compiles the project's content directory into the built mod's,
// replacing the previous one, and names the content addon in the manifest; a
// failed build leaves the content in place. A project without content ships
// none.
func (p *Project) buildContent() error {
	// Read each source, in the walk's lexical order.
	type source struct {
		kind, rest string
		data       []byte
	}
	var read []source
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
		read = append(read, source{kind, rest, data})
		return nil
	})
	if err != nil {
		return err
	}

	// Name the addon by a digest of the sources, so a release whose content
	// did not change keeps the addon a running game already mounted.
	p.Manifest.ContentAddon = ""
	if len(read) != 0 {
		digest := sha256.New()
		for _, src := range read {
			fmt.Fprintf(digest, "%s/%s\x00%d\x00", src.kind, src.rest, len(src.data))
			digest.Write(src.data)
		}
		p.Manifest.ContentAddon = check.AddonName(p.Manifest.GetSlug(), digest.Sum(nil))
	}
	addon := check.Addon(p.Manifest)

	// Place each source in the addon, naming the addon in it.
	sources := fstest.MapFS{}
	for _, src := range read {
		data := src.data
		switch path.Ext(src.rest) {
		case ".xml", ".js", ".css":
			data = addonToken.ReplaceAll(data, []byte("${1}"+addon+"/"))
		}
		place := path.Join(check.ContentDir, src.kind, addon, src.rest)
		if slices.Contains(check.AssetDirs, src.kind) {
			place = path.Join(src.kind, addon, src.rest)
		}
		sources[place] = &fstest.MapFile{Data: data}
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
