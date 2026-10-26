// Package fileretry retries a file operation that Windows refuses while
// another process holds the file for a moment, such as a game reading its
// configuration, a scanner opening a file just written, or a reader that did
// not share deletion. Other systems never refuse those operations, so the
// operation runs once there.
package fileretry

import (
	"os"
	"time"
)

const (
	// window bounds how long Do keeps retrying a busy file.
	window = 5 * time.Second
	// firstWait is the wait after the first refusal; it doubles up to maxWait.
	firstWait = 25 * time.Millisecond
	// maxWait bounds the wait between attempts.
	maxWait = 500 * time.Millisecond
)

// Do runs op, and runs it again while it fails because another process holds
// the file, for up to five seconds. It returns op's last error. op must leave
// the file as it found it when it fails, as a whole read, write or rename
// does.
func Do(op func() error) error {
	deadline := time.Now().Add(window)
	for wait := firstWait; ; wait = min(wait*2, maxWait) {
		err := op()
		if err == nil || !busy(err) || time.Now().Add(wait).After(deadline) {
			return err
		}
		time.Sleep(wait)
	}
}

// Rename renames oldpath to newpath as os.Rename does, retrying while another
// process holds either one, as Do does.
func Rename(oldpath, newpath string) error {
	return Do(func() error { return os.Rename(oldpath, newpath) })
}
