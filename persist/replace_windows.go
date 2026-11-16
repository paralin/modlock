//go:build windows

package persist

import (
	"golang.org/x/sys/windows"
)

// replace renames from over to and returns once the rename is on the disk.
// Windows cannot sync a directory as Unix does; MOVEFILE_WRITE_THROUGH makes
// MoveFileEx flush the move itself before it returns, which os.Rename does
// not ask for.
func replace(from, to string) error {
	source, err := windows.UTF16PtrFromString(from)
	if err != nil {
		return err
	}
	target, err := windows.UTF16PtrFromString(to)
	if err != nil {
		return err
	}
	return windows.MoveFileEx(source, target, windows.MOVEFILE_REPLACE_EXISTING|windows.MOVEFILE_WRITE_THROUGH)
}

// syncDir does nothing on Windows: a directory entry the store creates there
// is made durable by NTFS's metadata journal, and replace writes its move
// through.
var syncDir = func(string) error { return nil }
