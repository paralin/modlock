//go:build windows

package persist

import (
	"os"
	"path/filepath"

	"golang.org/x/sys/windows"
)

// replace renames from over to in the store directory at dir and returns
// once the rename is on the disk. It renames by path, as os.Root cannot ask
// for a write-through move; from was made new through the root, and a link
// at to is replaced, not followed.
// Windows cannot sync a directory as Unix does; MOVEFILE_WRITE_THROUGH makes
// MoveFileEx flush the move itself before it returns, which os.Rename does
// not ask for.
func replace(_ *os.Root, dir, from, to string) error {
	source, err := windows.UTF16PtrFromString(filepath.Join(dir, from))
	if err != nil {
		return err
	}
	target, err := windows.UTF16PtrFromString(filepath.Join(dir, to))
	if err != nil {
		return err
	}
	return windows.MoveFileEx(source, target, windows.MOVEFILE_REPLACE_EXISTING|windows.MOVEFILE_WRITE_THROUGH)
}

// syncDir does nothing on Windows: a directory entry the store creates there
// is made durable by NTFS's metadata journal, and replace writes its move
// through.
var syncDir = func(string, *os.Root) error { return nil }
