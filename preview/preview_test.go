package preview

import (
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"testing"

	"github.com/paralin/modlock/source2/panorama"
)

// TestScriptServesTheBuiltSource checks the page answers a layout's script
// include with the script's source from the build, and nothing outside the
// build's scripts.
func TestScriptServesTheBuiltSource(t *testing.T) {
	// Build one script into a mod's content addon.
	built := t.TempDir()
	source := []byte("Modlock.data((value) => $.Msg(value))\n")
	compiled, err := panorama.Compile("arena-1a2b", "panorama/scripts/board.js", source)
	if err != nil {
		t.Fatal(err)
	}
	dir := filepath.Join(built, "panorama", "scripts", "arena-1a2b")
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "board.vjs_c"), compiled, 0o644); err != nil {
		t.Fatal(err)
	}
	page := New(t.TempDir(), built, false, nil)

	// get answers one loopback request from the page itself.
	get := func(path string) *httptest.ResponseRecorder {
		request := httptest.NewRequest(http.MethodGet, "http://localhost:4000"+path, nil)
		answer := httptest.NewRecorder()
		page.ServeHTTP(answer, request)
		return answer
	}

	// The include's path names the addon and the script's source path.
	answer := get("/scripts/arena-1a2b/board.js")
	if answer.Code != http.StatusOK || answer.Body.String() != string(source) {
		t.Fatalf("board.js: %d %q", answer.Code, answer.Body.String())
	}
	if kind := answer.Header().Get("Content-Type"); kind != "text/javascript; charset=utf-8" {
		t.Fatalf("board.js type %q", kind)
	}

	// A missing script, another kind of file and a path out of the addon find
	// nothing.
	for _, path := range []string{
		"/scripts/arena-1a2b/missing.js",
		"/scripts/arena-1a2b/board.css",
		"/scripts/board.js",
		"/scripts/../scripts/arena-1a2b/board.js",
	} {
		if answer := get(path); answer.Code == http.StatusOK {
			t.Fatalf("%s answered %q", path, answer.Body.String())
		}
	}
}
