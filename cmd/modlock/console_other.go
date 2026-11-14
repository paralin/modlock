//go:build !windows

package main

// ownConsole reports false here: only Windows makes a console window for a
// program opened from its file manager.
func ownConsole() bool {
	return false
}
