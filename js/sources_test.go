package js

import (
	"io/fs"
	"path"
	"regexp"
	"strings"
	"testing"
)

// relativeImport matches a TypeScript import or export from a relative path.
// It reads only lines that begin a statement, or close a multi-line one, so a
// usage example in a comment, which imports from the mod's own files, is not
// taken for an import of the library's.
var relativeImport = regexp.MustCompile(`(?m)^\s*(?:import\b|export\b|\})[^\n]*?\bfrom '(\.{1,2}/[^']+)\.js'`)

// TestSourcesCloseOverImports checks that Sources holds the library's entry
// module and every module its source imports from inside src, so a vendored
// copy type checks. Imports that leave src, such as the protobuf messages,
// are vendored with their own packages.
func TestSourcesCloseOverImports(t *testing.T) {
	if _, err := fs.Stat(Sources, "src/index.ts"); err != nil {
		t.Fatal("Sources does not embed the entry module", err)
	}
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
			if !strings.HasPrefix(target, "src/") {
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
