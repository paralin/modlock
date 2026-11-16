//go:build !windows

package persist

import (
	"os"
	"path/filepath"
)

// replace renames from over to, then syncs the directory holding both, so
// the new name survives an OS crash or power loss once replace returns. A
// rename alone changes only the directory in memory: until the directory is
// synced, a crash can bring back the old file after the update was answered.
// On macOS, File.Sync asks the disk to flush as well (F_FULLFSYNC).
func replace(from, to string) error {
	if err := os.Rename(from, to); err != nil {
		return err
	}
	return syncDir(filepath.Dir(to))
}

// syncDir makes the entries of dir durable: the names created, renamed and
// removed in it. A test replaces it to see when it runs.
var syncDir = func(dir string) error {
	d, err := os.Open(dir)
	if err != nil {
		return err
	}
	if err := d.Sync(); err != nil {
		_ = d.Close()
		return err
	}
	return d.Close()
}
