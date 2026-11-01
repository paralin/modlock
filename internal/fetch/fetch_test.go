package fetch

import (
	"os"
	"path/filepath"
	"sync"
	"testing"
)

// TestInstallConcurrent installs one directory from several goroutines at
// once, each filling it slowly, and checks that every install succeeds and
// leaves one whole directory and no staging behind.
func TestInstallConcurrent(t *testing.T) {
	parent := t.TempDir()
	dir := filepath.Join(parent, "tool")

	// Hold every fill until all have started, so they overlap.
	const installs = 4
	var started, wg sync.WaitGroup
	started.Add(installs)
	errs := make([]error, installs)
	for i := range installs {
		wg.Go(func() {
			errs[i] = Install(dir, func(staging string) error {
				started.Done()
				started.Wait()
				if err := os.MkdirAll(staging, 0o755); err != nil {
					return err
				}
				return os.WriteFile(filepath.Join(staging, "tool"), []byte("tool"), 0o644)
			})
		})
	}
	wg.Wait()

	for i, err := range errs {
		if err != nil {
			t.Errorf("install %d: %v", i, err)
		}
	}
	if data, err := os.ReadFile(filepath.Join(dir, "tool")); err != nil || string(data) != "tool" {
		t.Fatalf("installed tool = %q, %v", data, err)
	}
	entries, err := os.ReadDir(parent)
	if err != nil || len(entries) != 1 {
		t.Fatalf("parent holds %v, %v; want the installed directory alone", entries, err)
	}
}
