//go:build !windows

package session

import (
	"os"
	"os/exec"
	"path/filepath"
)

// steamRoots lists where Steam may be installed: the native locations, then
// the private homes of the Flatpak and Snap packages, then macOS's.
func steamRoots() []string {
	home, _ := os.UserHomeDir()
	return []string{
		filepath.Join(home, ".local", "share", "Steam"),
		filepath.Join(home, ".steam", "steam"),
		filepath.Join(home, ".var", "app", "com.valvesoftware.Steam", ".local", "share", "Steam"),
		filepath.Join(home, "snap", "steam", "common", ".local", "share", "Steam"),
		filepath.Join(home, "Library", "Application Support", "Steam"),
	}
}

// steamExecutable returns the Steam client: steam on the PATH, or the
// installation's own launcher.
func steamExecutable(root string) string {
	if path, err := exec.LookPath("steam"); err == nil {
		return path
	}
	return filepath.Join(root, "steam.sh")
}
