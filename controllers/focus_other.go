//go:build !windows

package controllers

// focusWatch reports the game focused always: Deadlock runs on Windows, and
// on Linux under Proton, where the reader does not yet track its window.
type focusWatch struct{}

// Focused reports true.
func (f *focusWatch) Focused() bool {
	return true
}
