package main

import (
	"context"
	"io/fs"
	"os"
	"path/filepath"
	"runtime"
	"strings"

	"github.com/aperturerobotics/cli"
	modcli "github.com/paralin/modlock/cli"
	"github.com/paralin/modlock/internal/fetch"
	"github.com/paralin/modlock/internal/selfupdate"
	"github.com/pkg/errors"
)

// updateCommand installs a newer build of the command line over itself: the
// newest release, or the newest build of the master branch on that channel.
func updateCommand(out *printer) *cli.Command {
	return &cli.Command{
		Name:    "update",
		Aliases: []string{"upgrade"},
		Usage:   "install the newest modlock over this one, from the releases or the master branch",
		Flags: []cli.Flag{
			&cli.BoolFlag{Name: "check", Usage: "only say whether a newer build is out"},
			&cli.StringFlag{Name: "release", Usage: "install this release, such as v0.1.9, even an older one"},
			&cli.StringFlag{Name: "channel", Usage: "follow stable, the releases, or master, the newest commit's build, from now on"},
			&cli.BoolFlag{Name: "from-source", Usage: "build the master branch on this computer instead of downloading a build"},
		},
		Action: func(c *cli.Context) error {
			// Follow the channel asked for from now on, else the one kept.
			config, path, err := readConfig()
			if err != nil {
				return err
			}
			channel := updateChannel(config)
			if name := c.String("channel"); name != "" {
				if channel, err = selfupdate.ParseChannel(name); err != nil {
					return errors.Wrap(err, "--channel")
				}
			}
			if !c.Bool("check") && string(channel) != string(updateChannel(config)) {
				config.Channel = string(channel)
				if err := writeConfig(path, config); err != nil {
					return err
				}
				out.note("modlock update follows", string(channel), "from now on")
			}

			// A source build finds the newest commit itself.
			if c.Bool("from-source") {
				return updateFromSource(c.Context, out)
			}

			// Choose the build: the release asked for, or the channel's newest.
			target := c.String("release")
			if target == "" {
				latest, err := channel.Latest(c.Context)
				if err != nil {
					return err
				}
				selfupdate.Remember(channel, latest)
				target = latest
			}
			if !fetch.Published(target) {
				return errors.Errorf("--release names %q, which is not a release tag such as v0.1.9", target)
			}

			// Stop when this is already the build, or only a check was asked.
			if target == Version {
				out.mark("✓", green, "modlock "+Version+" is the newest "+buildOf(channel))
				return nil
			}
			if c.Bool("check") && out.json {
				out.update(target)
				return nil
			}
			if c.Bool("check") {
				out.mark("!", yellow, "modlock "+target+" is out, and this is "+Version+"; run modlock update to install it")
				return nil
			}

			// Replace this executable with the build's. A push to master can
			// replace its build during the download, so follow it once.
			exe, err := updatable(target)
			if err != nil {
				return err
			}
			out.mark("…", dim, "downloading modlock "+target+" for "+runtime.GOOS+" "+runtime.GOARCH)
			err = selfupdate.Install(c.Context, target, exe)
			if errors.Is(err, fetch.ErrSuperseded) {
				if target, err = channel.Latest(c.Context); err != nil {
					return err
				}
				err = selfupdate.Install(c.Context, target, exe)
			}
			if err != nil {
				return explainInstall(err, exe)
			}
			out.mark("✓", green, "updated "+exe+" from "+Version+" to "+target)
			return nil
		},
	}
}

// updateChannel returns the channel the settings follow, which is stable
// unless they name another. A name no longer known also means stable.
func updateChannel(config *modcli.Config) selfupdate.Channel {
	channel, err := selfupdate.ParseChannel(config.GetChannel())
	if err != nil {
		return selfupdate.Stable
	}
	return channel
}

// buildOf names what a channel's versions are.
func buildOf(channel selfupdate.Channel) string {
	if channel == selfupdate.Master {
		return "build of master"
	}
	return "release"
}

// updatable returns the executable an update to target replaces, or how to
// update a command line that a package manager installed.
func updatable(target string) (string, error) {
	exe, err := selfupdate.Executable()
	if err != nil {
		return "", err
	}
	if strings.Contains(filepath.ToSlash(exe), "/node_modules/@modlock/") {
		if !selfupdate.Released(target) {
			return "", errors.Errorf("npm installed %s, and npm carries only releases; install modlock from its release page to follow master", exe)
		}
		return "", errors.Errorf("npm installed %s; run npm install @modlock/modlock@%s to update it", exe, strings.TrimPrefix(target, "v"))
	}
	return exe, nil
}

// updateFromSource builds the master branch on this computer and installs
// the result over this command line.
func updateFromSource(ctx context.Context, out *printer) error {
	// Find the executable to replace.
	exe, err := updatable(string(selfupdate.Master))
	if err != nil {
		return err
	}

	// Build over it, showing the tools' output.
	out.mark("…", dim, "building modlock from the master branch; the first build compiles the game server, which takes several minutes")
	version, err := selfupdate.Build(ctx, exe, os.Stderr)
	if err != nil {
		return explainInstall(err, exe)
	}
	out.mark("✓", green, "updated "+exe+" from "+Version+" to "+version+", built from source")
	return nil
}

// explainInstall names the next step for an update that could not replace
// exe.
func explainInstall(err error, exe string) error {
	if !errors.Is(err, fs.ErrPermission) {
		return err
	}
	if runtime.GOOS == "windows" {
		return errors.Wrapf(err, "modlock cannot replace %s; run modlock update from a terminal opened as administrator", exe)
	}
	return errors.Wrapf(err, "modlock cannot replace %s; run sudo modlock update", exe)
}

// checkForUpdate looks for a newer build on the settings' channel in the background and tells out,
// unless the release is a development build, the command is one that reports
// releases itself, or the settings, MODLOCK_NO_UPDATE_CHECK or CI turn the
// check off.
func checkForUpdate(ctx context.Context, out *printer, command string) {
	// Skip the check where it is off, would repeat the command, or runs in
	// the background.
	if command == "update" || command == "upgrade" || command == "config" || command == "daemon" {
		return
	}
	if os.Getenv("MODLOCK_NO_UPDATE_CHECK") != "" || os.Getenv("CI") != "" {
		return
	}
	config, _, err := readConfig()
	if err != nil || config.GetSkipUpdateCheck() {
		return
	}

	// Look, from the cache at once and from GitHub at most daily.
	selfupdate.Check(ctx, updateChannel(config), Version, out.update)
}
