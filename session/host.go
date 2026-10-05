package session

import (
	"context"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strconv"

	"github.com/pkg/errors"
)

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
	// Steam is the Steam installation; on Linux its Proton runs the host.
	Steam *Steam
	// Output receives the host's console output.
	Output io.Writer
}

// Host is a running modlock-host process.
type Host struct {
	// cmd is the host process, or Proton's wrapper around it.
	cmd *exec.Cmd
	// proton runs the host on Linux, or is nil on Windows.
	proton *Proton
}

// StartHost starts modlock-host as a listen server with the mods. On Windows
// the host runs directly; on Linux it runs through Steam's Proton in a prefix
// of its own.
func StartHost(ctx context.Context, config HostConfig) (*Host, error) {
	// Choose how to run the Windows host on this system.
	var proton *Proton
	switch runtime.GOOS {
	case "windows":
	case "linux":
		found, err := FindProton(config.Steam)
		if err != nil {
			return nil, err
		}
		if err := found.Prepare(ctx); err != nil {
			return nil, err
		}
		proton = found
	default:
		return nil, errors.New("Deadlock servers run on Windows, or on Linux through Proton")
	}

	// Name every path as the host sees it.
	path := func(path string) string { return path }
	if proton != nil {
		path = WindowsPath
	}
	arguments := []string{
		"--game-dir", path(config.GameDir),
		"--hostport", strconv.Itoa(int(config.Port)),
		// An empty server hibernates and runs no frames, so mods would wait
		// for the first player.
		"--engine-args", "+sv_hibernate_when_empty 0",
	}
	if config.Map != "" {
		arguments = append(arguments, "--map", config.Map)
	}
	if config.Control != "" {
		arguments = append(arguments, "--control", config.Control)
	}
	for _, mod := range config.Mods {
		arguments = append(arguments, "--plugin", path(mod))
	}
	if len(config.Args) != 0 {
		arguments = append(append(arguments, "--"), config.Args...)
	}

	// Start the host in its own directory, where its libraries are.
	cmd := exec.CommandContext(ctx, config.Executable, arguments...)
	if proton != nil {
		cmd = proton.Command(ctx, config.Executable, arguments...)
	}
	cmd.Dir = filepath.Dir(config.Executable)
	if config.Output != nil {
		output := &consoleFilter{out: config.Output}
		cmd.Stdout = output
		cmd.Stderr = output
	}
	if err := cmd.Start(); err != nil {
		return nil, errors.Wrap(err, "start modlock-host")
	}
	return &Host{cmd: cmd, proton: proton}, nil
}

// Wait waits for the host to exit.
func (h *Host) Wait() error {
	return h.cmd.Wait()
}

// Stop ends the host and, under Proton, every process in its prefix.
func (h *Host) Stop() {
	if h.cmd.Process != nil {
		_ = h.cmd.Process.Kill()
	}
	if h.proton != nil {
		h.proton.Kill()
	}
}

// FindHost returns modlock-host.exe: MODLOCK_HOST when set, else the release
// matching version, downloaded once into the user cache.
func FindHost(ctx context.Context, version string) (string, error) {
	if path := os.Getenv("MODLOCK_HOST"); path != "" {
		return filepath.Abs(path)
	}
	return DownloadHost(ctx, version)
}
