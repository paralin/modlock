package panorama

import (
	"fmt"
	"io/fs"
	"strings"
	"testing/fstest"

	"github.com/pkg/errors"
)

// CompileTree compiles the addon content directory tree, such as
// content/citadel_addons/<addon>, into the addon's game files: each layout,
// script and style sheet under panorama/ becomes its compiled resource, and
// every other file is kept as it is, so already compiled resources, such as
// images, ship beside them. Each style sheet under panorama/styles/<dir>/ also
// gets its sheet layout (see SheetLayout).
func CompileTree(addon string, tree fs.FS) (fstest.MapFS, error) {
	out := fstest.MapFS{}
	add := func(name string, data []byte) error {
		if _, exists := out[name]; exists {
			return errors.Errorf("%s is both compiled from source and present as a file", name)
		}
		out[name] = &fstest.MapFile{Data: data, Mode: 0o644}
		return nil
	}
	err := fs.WalkDir(tree, ".", func(name string, entry fs.DirEntry, err error) error {
		// Read each regular file.
		if err != nil || !entry.Type().IsRegular() {
			return err
		}
		data, err := fs.ReadFile(tree, name)
		if err != nil {
			return err
		}

		// Keep everything outside Panorama's sources as it is.
		compiled, ok := Compiled(name)
		if !strings.HasPrefix(name, "panorama/") || !ok {
			return add(name, data)
		}

		// Compile the source, and a sheet's layout beside a sheet.
		if data, err = Compile(addon, name, data); err != nil {
			return err
		}
		if err := add(compiled, data); err != nil {
			return err
		}
		wrapper, source, ok := SheetLayout(name)
		if !ok {
			return nil
		}
		if data, err = Compile(addon, wrapper, source); err != nil {
			return err
		}
		compiled, _ = Compiled(wrapper)
		return add(compiled, data)
	})
	return out, err
}

// SheetLayout returns the layout the build writes beside the style sheet at
// name, such as panorama/layout/<dir>/hud.css.xml for
// panorama/styles/<dir>/hud.css, with its source, and whether name is such a
// sheet. The layout includes the sheet and draws nothing, so loading it into a
// panel styles the panels a script adds beneath it; Modlock's interface
// renderer loads a mod's sheets this way.
func SheetLayout(name string) (string, []byte, bool) {
	rest, ok := strings.CutPrefix(name, "panorama/styles/")
	if !ok || !strings.HasSuffix(rest, ".css") || !strings.Contains(rest, "/") {
		return "", nil, false
	}
	source := fmt.Sprintf("<root>\n  <styles>\n    <include src=\"file://{resources}/styles/%s\" />\n  </styles>\n  <Panel />\n</root>\n", rest)
	return "panorama/layout/" + rest + ".xml", []byte(source), true
}
