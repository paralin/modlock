//go:build !windows

package fileretry

// busy reports false: only Windows refuses a file another process holds.
func busy(error) bool { return false }
