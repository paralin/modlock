//go:build windows

package filelock

import (
	"os"

	"github.com/pkg/errors"
	"golang.org/x/sys/windows"
)

// Lock opens path and locks it, exclusively or shared with other shared
// holders. With wait false it returns ErrLocked at once when the lock is
// held against it. Closing the file releases the lock, and so does the
// process exiting.
func Lock(path string, exclusive, wait bool) (*os.File, error) {
	// Open the lock file, creating it once.
	f, err := os.OpenFile(path, os.O_RDWR|os.O_CREATE, 0o600)
	if err != nil {
		return nil, err
	}
	if err := LockOpen(f, exclusive, wait); err != nil {
		_ = f.Close()
		return nil, err
	}
	return f, nil
}

// LockOpen locks the open file f as Lock does, for a caller that opened the
// file itself. It leaves f open when the lock fails.
func LockOpen(f *os.File, exclusive, wait bool) error {
	// Take the lock over the file's first byte in its mode, waiting for it
	// only when asked.
	var flags uint32
	if exclusive {
		flags |= windows.LOCKFILE_EXCLUSIVE_LOCK
	}
	if !wait {
		flags |= windows.LOCKFILE_FAIL_IMMEDIATELY
	}
	err := windows.LockFileEx(windows.Handle(f.Fd()), flags, 0, 1, 0, &windows.Overlapped{})
	if errors.Is(err, windows.ERROR_LOCK_VIOLATION) {
		return ErrLocked
	}
	return err
}
