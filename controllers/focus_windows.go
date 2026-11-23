package controllers

import (
	"path/filepath"
	"strings"

	"golang.org/x/sys/windows"
)

// gameImage is the file name of Deadlock's game process.
const gameImage = "deadlock.exe"

// focusWatch reports whether Deadlock's window is in the foreground, and
// remembers the answer for the last foreground window so a check costs one
// call while focus stays put.
type focusWatch struct {
	window  windows.HWND
	focused bool
}

// Focused reports whether the foreground window belongs to Deadlock.
func (f *focusWatch) Focused() bool {
	window := windows.GetForegroundWindow()
	if window == f.window && window != 0 {
		return f.focused
	}
	f.window, f.focused = window, window != 0 && isGame(window)
	return f.focused
}

// isGame reports whether window's process runs Deadlock's game image.
func isGame(window windows.HWND) bool {
	var pid uint32
	if _, err := windows.GetWindowThreadProcessId(window, &pid); err != nil || pid == 0 {
		return false
	}
	process, err := windows.OpenProcess(windows.PROCESS_QUERY_LIMITED_INFORMATION, false, pid)
	if err != nil {
		return false
	}
	defer windows.CloseHandle(process)
	buf := make([]uint16, windows.MAX_PATH)
	size := uint32(len(buf))
	if err := windows.QueryFullProcessImageName(process, 0, &buf[0], &size); err != nil {
		return false
	}
	return strings.EqualFold(filepath.Base(windows.UTF16ToString(buf[:size])), gameImage)
}
