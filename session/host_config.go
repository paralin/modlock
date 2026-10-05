package session

import "io"

// HostConfig describes one modlock-host run.
type HostConfig struct {
	// Executable is modlock-host.exe, beside its libraries.
	Executable string
	// GameDir is the Deadlock installation directory.
	GameDir string
	// Mods lists the built mods to load, as directories with mod.json.
	Mods []string
	// Control is the address of the controller's Control link.
	Control string
	// Port is the server's UDP port.
	Port uint16
	// Map is the map the server starts, or empty for the host's default.
	Map string
	// Args are passed to each mod's start handlers.
	Args []string
	// Settings is the file that keeps players' settings, or empty to keep
	// them for the run only.
	Settings string
	// Steam is the Steam installation; on Linux its Proton runs the host.
	Steam *Steam
	// Output receives the host's console output.
	Output io.Writer
}
