//go:build windows

package fileretry

import (
	"errors"

	"golang.org/x/sys/windows"
)

// busy reports whether err is Windows refusing a file another process holds
// open. Windows reports a rename over an open file as access denied.
func busy(err error) bool {
	return errors.Is(err, windows.ERROR_SHARING_VIOLATION) ||
		errors.Is(err, windows.ERROR_LOCK_VIOLATION) ||
		errors.Is(err, windows.ERROR_ACCESS_DENIED)
}
