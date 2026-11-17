//go:build !windows

package filelock

import (
	"os"

	"github.com/pkg/errors"
	"golang.org/x/sys/unix"
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
	// Take the lock in its mode, waiting for it only when asked.
	how := unix.LOCK_SH
	if exclusive {
		how = unix.LOCK_EX
	}
	if !wait {
		how |= unix.LOCK_NB
	}
	var err error
	for {
		err = unix.Flock(int(f.Fd()), how)
		if err != unix.EINTR {
			break
		}
	}
	if errors.Is(err, unix.EWOULDBLOCK) {
		return ErrLocked
	}
	return err
}
