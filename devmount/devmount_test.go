package devmount

import (
	"os"
	"path/filepath"
	"testing"
)

// TestClearStale walks a mount through its owners: a live session keeps it,
// a session that ended without unmounting leaves it for ClearStale, and a
// clean installation is left untouched.
func TestClearStale(t *testing.T) {
	gameDir := t.TempDir()
	info := filepath.Join(gameDir, "game", "citadel", "gameinfo.gi")
	if err := os.MkdirAll(filepath.Dir(info), 0o755); err != nil {
		t.Fatal(err)
	}
	stock := "\"GameInfo\"\n{\n\tFileSystem\n\t{\n\t\tSearchPaths\n\t\t{\n\t\t\tGame\t\t\t\tcitadel\n\t\t}\n\t}\n}\n"
	if err := os.WriteFile(info, []byte(stock), 0o644); err != nil {
		t.Fatal(err)
	}
	read := func() string {
		data, err := os.ReadFile(info)
		if err != nil {
			t.Fatal(err)
		}
		return string(data)
	}

	// A clean installation has nothing to clear.
	if err := ClearStale(gameDir); err != nil {
		t.Fatal(err)
	}

	// A live session keeps its mount.
	held, err := Hold(gameDir)
	if err != nil {
		t.Fatal(err)
	}
	if err := Mount(gameDir, "package"); err != nil {
		t.Fatal(err)
	}
	if err := ClearStale(gameDir); err != ErrRunning {
		t.Fatalf("clear beside a live session: %v", err)
	}
	if read() == stock {
		t.Fatal("a live session lost its mount")
	}

	// A session that ended without unmounting leaves a stale block.
	if err := held.Close(); err != nil {
		t.Fatal(err)
	}
	if err := ClearStale(gameDir); err != nil {
		t.Fatal(err)
	}
	if got := read(); got != stock {
		t.Fatalf("stale block kept:\n%s", got)
	}

	// A block from before the lock file counts as stale too.
	if err := os.Remove(filepath.Join(Path(gameDir), lockName)); err != nil {
		t.Fatal(err)
	}
	if err := Mount(gameDir, "package"); err != nil {
		t.Fatal(err)
	}
	if err := ClearStale(gameDir); err != nil || read() != stock {
		t.Fatalf("unlocked block kept: %v", err)
	}
}
