//go:build !windows

package persist

import (
	"errors"
	"os"
	"path/filepath"
	"slices"
	"strings"
	"testing"

	persistpb "github.com/paralin/modlock/src/wasm/persist"
)

// TestStoreSyncsDirectory answers an update only after the directory that
// holds the renamed file is synced, so an OS crash cannot bring back the old
// file, and refuses to answer one whose sync failed. Open syncs the
// directory it creates the store's directory in.
func TestStoreSyncsDirectory(t *testing.T) {
	// Note each directory synced, and that the renamed file is in place then.
	var synced []string
	saved := syncDir
	t.Cleanup(func() { syncDir = saved })
	syncDir = func(dir string, root *os.Root) error {
		synced = append(synced, dir)
		return saved(dir, root)
	}

	// Open creates the store's directory and syncs its parent.
	root := t.TempDir()
	dir := filepath.Join(root, "prophunt")
	store, err := Open(dir)
	if err != nil {
		t.Fatal(err)
	}
	if !slices.Equal(synced, []string{root}) {
		t.Fatalf("open synced %q, want %q", synced, root)
	}

	// An update syncs the store's directory after its rename.
	synced = nil
	file := filepath.Join(dir, "76561197960265728.json")
	syncDir = func(d string, root *os.Root) error {
		if data, err := os.ReadFile(file); err != nil || !strings.Contains(string(data), "roundsWon") {
			t.Errorf("synced %s before the rename: %v", d, err)
		}
		synced = append(synced, d)
		return saved(d, root)
	}
	if _, _, err := store.Update(76561197960265728, stats, []*persistpb.Op{add("roundsWon", 1)}, "round-1"); err != nil {
		t.Fatal(err)
	}
	if !slices.Equal(synced, []string{dir}) {
		t.Fatalf("update synced %q, want %q", synced, dir)
	}

	// An update whose directory sync failed is not answered as kept.
	syncDir = func(string, *os.Root) error { return errors.New("the disk went away") }
	if _, _, err := store.Update(76561197960265728, stats, []*persistpb.Op{add("roundsWon", 1)}, "round-2"); err == nil {
		t.Fatal("an update whose directory sync failed was answered as kept")
	}
}

// TestStoreRetrySyncsDirectory answers a resend of an update whose rename
// landed but whose directory sync failed only once the directory is synced,
// so the repeat is never acknowledged ahead of the durability barrier.
func TestStoreRetrySyncsDirectory(t *testing.T) {
	// An update renames its file in, then its directory sync fails.
	saved := syncDir
	t.Cleanup(func() { syncDir = saved })
	dir := filepath.Join(t.TempDir(), "prophunt")
	store, err := Open(dir)
	if err != nil {
		t.Fatal(err)
	}
	calls := 0
	syncDir = func(string, *os.Root) error {
		calls++
		return errors.New("the disk went away")
	}
	ops := []*persistpb.Op{add("roundsWon", 1)}
	if _, _, err := store.Update(76561197960265728, stats, ops, "round-1"); err == nil {
		t.Fatal("an update whose directory sync failed was answered as kept")
	}

	// A resend while the sync still fails is refused, after trying it.
	calls = 0
	if _, repeated, err := store.Update(76561197960265728, stats, ops, "round-1"); err == nil {
		t.Fatalf("a resend was answered repeated=%v with directory sync calls=%d while the sync fails", repeated, calls)
	}
	if calls != 1 {
		t.Fatalf("a resend tried the directory sync %d times, want 1", calls)
	}

	// Once the sync succeeds the resend is answered as a repeat, counted once.
	calls = 0
	syncDir = func(d string, root *os.Root) error {
		calls++
		return saved(d, root)
	}
	record, repeated, err := store.Update(76561197960265728, stats, ops, "round-1")
	if err != nil || !repeated || calls != 1 || record.GetFields()["roundsWon"].GetNumber() != 1 {
		t.Fatalf("resend: repeated=%v calls=%d record=%v err=%v", repeated, calls, record, err)
	}
}
