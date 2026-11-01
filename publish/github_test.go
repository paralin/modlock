//go:build !windows

package publish

import (
	"io"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// TestGitHubPublishRecovery runs the real publisher against a subprocess
// fixture for GitHub's API boundary, including destructive clobber failure.
func TestGitHubPublishRecovery(t *testing.T) {
	// A fake gh executable models the remote release and asset as real files.
	dir := t.TempDir()
	script := `#!/bin/sh
set -eu
root=$MODLOCK_TEST_REMOTE
case "$1 $2" in
  'release view')
    test -f "$root/state" || exit 1
    test "$(cat "$root/state")" = draft && echo true || echo false
    ;;
  'release create')
    echo draft > "$root/state"
    test "${MODLOCK_TEST_FAILURE:-}" != create || exit 1
    cp "$4" "$root/asset"
    ;;
  'release upload')
    rm -f "$root/asset"
    if test "${MODLOCK_TEST_FAILURE:-}" = restore; then exit 2; fi
    if test "${MODLOCK_TEST_FAILURE:-}" = upload && test "$4" = "$MODLOCK_TEST_ARCHIVE"; then exit 1; fi
    cp "$4" "$root/asset"
    ;;
  'release edit')
    test -f "$root/asset"
    echo public > "$root/state"
    ;;
  api\ *)
    case "$2" in
      */releases/tags/*) test ! -f "$root/asset" || echo 42 ;;
      */releases/assets/42) cat "$root/asset" ;;
      *) exit 3 ;;
    esac
    ;;
  *) exit 4 ;;
esac
`
	if err := os.WriteFile(filepath.Join(dir, "gh"), []byte(script), 0o700); err != nil {
		t.Fatal(err)
	}

	// Bind publishing and its remote fixture to this test's files.
	t.Setenv("PATH", dir+string(os.PathListSeparator)+os.Getenv("PATH"))
	t.Setenv("MODLOCK_TEST_REMOTE", dir)
	archive := filepath.Join(dir, "mod.zip")
	t.Setenv("MODLOCK_TEST_ARCHIVE", archive)
	if err := os.WriteFile(archive, []byte("new"), 0o600); err != nil {
		t.Fatal(err)
	}
	publish := func() error { return GitHub(t.Context(), dir, "v1", archive, "notes", io.Discard) }
	read := func(name string) string {
		t.Helper()
		data, err := os.ReadFile(filepath.Join(dir, name))
		if err != nil {
			t.Fatal(err)
		}
		return strings.TrimSpace(string(data))
	}

	// A failed first upload leaves a draft; retry completes and publishes it.
	t.Setenv("MODLOCK_TEST_FAILURE", "create")
	if err := publish(); err == nil || read("state") != "draft" {
		t.Fatal("failed create exposed an incomplete release", err)
	}
	t.Setenv("MODLOCK_TEST_FAILURE", "")
	if err := publish(); err != nil || read("state") != "public" || read("asset") != "new" {
		t.Fatal("retry did not publish the completed draft", err)
	}

	// A failed clobber restores the previous bytes, even after deleting them.
	if err := os.WriteFile(archive, []byte("replacement"), 0o600); err != nil {
		t.Fatal(err)
	}
	t.Setenv("MODLOCK_TEST_FAILURE", "upload")
	if err := publish(); err == nil || read("asset") != "new" {
		t.Fatal("failed clobber lost the previous asset", err)
	}
	t.Setenv("MODLOCK_TEST_FAILURE", "")
	if err := publish(); err != nil || read("asset") != "replacement" {
		t.Fatal("clobber could not replace the existing version", err)
	}

	// A failed restore keeps the recovery file and reports where it is.
	t.Setenv("MODLOCK_TEST_FAILURE", "restore")
	err := publish()
	if err == nil {
		t.Fatal("failed replacement and restoration reported success")
	}

	// The reported recovery path must hold the last published bytes.
	_, path, found := strings.Cut(err.Error(), "previous asset kept at ")
	if !found {
		t.Fatal("failed restoration omitted its recovery path", err)
	}
	path, _, _ = strings.Cut(path, ": exit status")
	defer os.RemoveAll(filepath.Dir(path))
	old, err := os.ReadFile(path)
	if err != nil || string(old) != "replacement" {
		t.Fatal("failed restoration lost its recovery copy", err)
	}
}
