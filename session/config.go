package session

import (
	"io"

	"github.com/paralin/modlock/proto/modlock/control"
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
	// Launch starts the Deadlock client once the server is ready.
	Launch bool
	// HostOutput receives the host's console output.
	HostOutput io.Writer
	// Interface is the loopback address the player's game reads the mods'
	// interfaces from, or empty to serve none.
	Interface string
	// Relay is the origin of the https page the game reads them through.
	Relay string
	// Events receives each host event, on one goroutine.
	Events func(*control.HostEvent)
}
