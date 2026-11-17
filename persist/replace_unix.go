//go:build !windows

package persist

import "os"

// replace renames from over to in the store directory root, which is at dir,
// then syncs the directory, so the new name survives an OS crash or power
// loss once replace returns. A rename alone changes only the directory in
// memory: until the directory is synced, a crash can bring back the old file
// after the update was answered. On macOS, File.Sync asks the disk to flush
// as well (F_FULLFSYNC).
func replace(root *os.Root, dir, from, to string) error {
	if err := root.Rename(from, to); err != nil {
		return err
	}
	return syncDir(dir, root)
}

// syncDir makes the entries of dir durable: the names created, renamed and
// removed in it. It opens root when it is not nil, the store's own directory,
// and dir by its path otherwise. A test replaces it to see when it runs.
var syncDir = func(dir string, root *os.Root) error {
	// Open the directory through root when there is one, then sync it.
	var d *os.File
	var err error
	if root != nil {
		d, err = root.Open(".")
	} else {
		d, err = os.Open(dir)
	}
	if err != nil {
		return err
	}
	if err := d.Sync(); err != nil {
		_ = d.Close()
		return err
	}
	return d.Close()
}
