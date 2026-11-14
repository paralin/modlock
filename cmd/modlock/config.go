package main

import (
	"fmt"
	"os"
	"path/filepath"
	"slices"
	"strings"

	"github.com/aperturerobotics/cli"
	modcli "github.com/paralin/modlock/cli"
	"github.com/paralin/modlock/internal/selfupdate"
	"github.com/pkg/errors"
)

// setting is one entry of the command line's configuration, as modlock config
// names and shows it.
type setting struct {
	// name is the setting's name on the command line.
	name string
	// usage says what the setting does and which values it takes.
	usage string
	// get returns the setting's value in config.
	get func(config *modcli.Config) string
	// set changes the setting in config to value.
	set func(config *modcli.Config, value string) error
}

// settings are the entries modlock config lists and changes.
var settings = []setting{{
	name:  "update-check",
	usage: "on to say when a newer modlock is out, off to stop looking",
	get: func(config *modcli.Config) string {
		return onOff(!config.GetSkipUpdateCheck())
	},
	set: func(config *modcli.Config, value string) error {
		on, err := parseOnOff(value)
		config.SkipUpdateCheck = !on
		return err
	},
}, {
	name:  "editor-offer",
	usage: "on to offer the editor extension once, off to never ask",
	get: func(config *modcli.Config) string {
		return onOff(!config.GetEditorOffered())
	},
	set: func(config *modcli.Config, value string) error {
		on, err := parseOnOff(value)
		config.EditorOffered = !on
		return err
	},
}, {
	name:  "channel",
	usage: "stable to update to releases, master to follow the master branch's newest build",
	get: func(config *modcli.Config) string {
		return string(updateChannel(config))
	},
	set: func(config *modcli.Config, value string) error {
		channel, err := selfupdate.ParseChannel(value)
		config.Channel = string(channel)
		return err
	},
}}

// configCommand lists and changes the command line's settings.
func configCommand(out *printer) *cli.Command {
	return &cli.Command{
		Name:  "config",
		Usage: "list the command line's settings, or change one with set",
		Action: func(c *cli.Context) error {
			// Read the settings.
			config, path, err := readConfig()
			if err != nil {
				return err
			}

			// List each one with its value, then the file that keeps them,
			// in aligned columns.
			width := len("file")
			for _, entry := range settings {
				width = max(width, len(entry.name))
			}
			for _, entry := range settings {
				out.field(fmt.Sprintf("%-*s", width, entry.name), fmt.Sprintf("%-6s %s", entry.get(config), paint(out.color, dim, entry.usage)))
			}
			out.field(fmt.Sprintf("%-*s", width, "file"), path)
			return nil
		},
		Subcommands: []*cli.Command{{
			Name:      "set",
			Usage:     "change a setting, such as: modlock config set update-check off",
			ArgsUsage: "NAME VALUE",
			Action: func(c *cli.Context) error {
				// Find the setting.
				if c.NArg() != 2 {
					return errors.New("name a setting and its value, such as: modlock config set update-check off")
				}
				at := slices.IndexFunc(settings, func(entry setting) bool { return entry.name == c.Args().Get(0) })
				if at < 0 {
					return errors.Errorf("modlock has no setting %q; modlock config lists them", c.Args().Get(0))
				}
				entry := settings[at]

				// Change it and save the file.
				config, path, err := readConfig()
				if err != nil {
					return err
				}
				if err := entry.set(config, c.Args().Get(1)); err != nil {
					return errors.Wrap(err, entry.name)
				}
				if err := writeConfig(path, config); err != nil {
					return err
				}
				out.note(entry.name, "is", entry.get(config))
				return nil
			},
		}},
	}
}

// configFile returns the file that keeps the settings.
func configFile() (string, error) {
	dir, err := os.UserConfigDir()
	if err != nil {
		return "", errors.Wrap(err, "find the configuration directory")
	}
	return filepath.Join(dir, "modlock", "config.json"), nil
}

// readConfig returns the settings and the file that keeps them. A missing file
// holds the defaults.
func readConfig() (*modcli.Config, string, error) {
	// Find and read the file.
	path, err := configFile()
	if err != nil {
		return nil, "", err
	}
	config := &modcli.Config{}
	data, err := os.ReadFile(path)
	if errors.Is(err, os.ErrNotExist) {
		return config, path, nil
	}
	if err != nil {
		return nil, "", errors.Wrap(err, "read the settings")
	}

	// Decode it.
	if err := config.UnmarshalJSON(data); err != nil {
		return nil, "", errors.Wrapf(err, "%s is not valid; correct or delete it", path)
	}
	return config, path, nil
}

// writeConfig saves config to path whole: a failed write leaves the old file.
func writeConfig(path string, config *modcli.Config) error {
	// Encode the settings.
	data, err := config.MarshalJSON()
	if err != nil {
		return errors.Wrap(err, "save the settings")
	}

	// Write them beside the file, then move them over it.
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		return errors.Wrap(err, "save the settings")
	}
	staged := path + ".tmp"
	if err := os.WriteFile(staged, append(data, '\n'), 0o644); err != nil {
		return errors.Wrap(err, "save the settings")
	}
	return errors.Wrap(os.Rename(staged, path), "save the settings")
}

// onOff names a switch's state.
func onOff(on bool) string {
	if on {
		return "on"
	}
	return "off"
}

// parseOnOff reads a switch's state from value, such as on, off, true or 0.
func parseOnOff(value string) (bool, error) {
	switch strings.ToLower(value) {
	case "on", "true", "yes", "1":
		return true, nil
	case "off", "false", "no", "0":
		return false, nil
	}
	return false, errors.Errorf("%q is neither on nor off", value)
}
