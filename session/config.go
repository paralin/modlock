package session

import (
	"io"

	"github.com/paralin/modlock/src/host_app/control"
)

// Config describes a play session.
type Config struct {
	// Version is the command line's release, whose host the session runs
	// unless MODLOCK_HOST names one.
	Version string
	// Mods lists the built mods to load.
	Mods []string
	// Port is the server's UDP port.
	Port uint16
	// Map is the map the server starts, or empty for the default.
	Map string
	// Args are passed to each mod's start handlers.
	Args []string
	// Settings is the file that keeps players' settings, or empty to keep
	// them for the session only.
	Settings string
	// Files is the directory that keeps the files mods write, or empty to
	// refuse their writes.
	Files string
	// Fixtures is the file of service fixtures the host answers from, a
	// control.Fixtures message, or empty for none.
	Fixtures string
	// Launch starts the Deadlock client once the server is ready.
	Launch bool
	// HostOutput receives the host's console output.
	HostOutput io.Writer
	// Interface is the loopback address the player's game reads the mods'
	// interfaces from, or empty to serve none.
	Interface string
	// Relay is the origin of the https page the game reads them through.
	Relay string
	// Renderer is the built renderer, which the session mounts in the game so
	// it draws the interfaces, or nil to leave the game unchanged.
	Renderer *Renderer
	// Events receives each host event, on one goroutine.
	Events func(*control.HostEvent)
}
