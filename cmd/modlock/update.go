package main

import (
	"context"
	"io/fs"
	"os"
	"path/filepath"
	"runtime"
	"strings"

	"github.com/aperturerobotics/cli"
	"github.com/paralin/modlock/internal/selfupdate"
	"github.com/pkg/errors"
)

// updateCommand installs a newer release of the command line over itself.
func updateCommand(out *printer) *cli.Command {
	return &cli.Command{
		Name:    "update",
		Aliases: []string{"upgrade"},
		Usage:   "install the newest modlock release over this one",
		Flags: []cli.Flag{
			&cli.BoolFlag{Name: "check", Usage: "only say whether a newer release is out"},
			&cli.StringFlag{Name: "release", Usage: "install this release, such as v0.1.9, even an older one"},
		},
		Action: func(c *cli.Context) error {
			// Choose the release: the one asked for, or the newest.
			target := c.String("release")
			if target == "" {
				latest, err := selfupdate.Latest(c.Context)
				if err != nil {
					return err
				}
				selfupdate.Remember(latest)
				target = latest
			}
			if !selfupdate.Released(target) {
				return errors.Errorf("--release names %q, which is not a release tag such as v0.1.9", target)
			}

			// Stop when this is already the release, or only a check was asked.
			if target == Version {
				out.mark("✓", green, "modlock "+Version+" is the newest release")
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

			// Replace this executable with the release's.
			exe, err := selfupdate.Executable()
			if err != nil {
				return err
			}
			if strings.Contains(filepath.ToSlash(exe), "/node_modules/@modlock/") {
				return errors.Errorf("npm installed %s; run npm install @modlock/modlock@%s to update it", exe, strings.TrimPrefix(target, "v"))
			}
			out.mark("…", dim, "downloading modlock "+target+" for "+runtime.GOOS+" "+runtime.GOARCH)
			if err := selfupdate.Install(c.Context, target, exe); err != nil {
				return explainInstall(err, exe)
			}
			out.mark("✓", green, "updated "+exe+" from "+Version+" to "+target)
			return nil
		},
	}
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

// checkForUpdate looks for a newer release in the background and tells out,
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
	if config, _, err := readConfig(); err == nil && config.GetSkipUpdateCheck() {
		return
	}

	// Look, from the cache at once and from GitHub at most daily.
	selfupdate.Check(ctx, Version, out.update)
}
