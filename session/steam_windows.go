package session

import (
	"path/filepath"

	"golang.org/x/sys/windows/registry"
)

// steamRoots lists where Steam may be installed, the registered one first.
func steamRoots() []string {
	roots := []string{`C:\Program Files (x86)\Steam`}
	key, err := registry.OpenKey(registry.CURRENT_USER, `Software\Valve\Steam`, registry.QUERY_VALUE)
	if err != nil {
		return roots
	}
	defer key.Close()
	path, _, err := key.GetStringValue("SteamPath")
	if err != nil {
		return roots
	}
	return append([]string{filepath.Clean(path)}, roots...)
}

// steamExecutable returns the Steam client in root.
func steamExecutable(root string) string {
	return filepath.Join(root, "steam.exe")
}
