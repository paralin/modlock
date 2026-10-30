package panorama

import (
	"io/fs"
	"path"
	"regexp"
	"strings"
	"testing"
)

// relativeImport matches a TypeScript import or export from a relative path.
var relativeImport = regexp.MustCompile(`from '(\.{1,2}/[^']+)\.js'`)

// TestSourcesCloseOverImports checks that every relative import of an
// embedded source inside this package is embedded too, so a vendored copy
// builds. Imports that leave the package, such as the ui protocol, are
// vendored with their own packages.
func TestSourcesCloseOverImports(t *testing.T) {
	err := fs.WalkDir(Sources, ".", func(name string, entry fs.DirEntry, err error) error {
		if err != nil || entry.IsDir() || !strings.HasSuffix(name, ".ts") {
			return err
		}
		source, err := fs.ReadFile(Sources, name)
		if err != nil {
			return err
		}
		for _, match := range relativeImport.FindAllStringSubmatch(string(source), -1) {
			target := path.Join(path.Dir(name), match[1]) + ".ts"
			if strings.HasPrefix(target, "../") {
				continue
			}
			if _, err := fs.Stat(Sources, target); err != nil {
				t.Errorf("%s imports %s, which Sources does not embed", name, target)
			}
		}
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}
}
