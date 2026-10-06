package main

import (
	"os"

	"github.com/aperturerobotics/cli"
	"github.com/paralin/modlock/project"
	"github.com/paralin/modlock/proto/modlock/wasm"
	"github.com/pkg/errors"
)

// languages maps each --language name to the manifest's language.
var languages = map[string]wasm.Manifest_Language{
	"typescript": wasm.Manifest_LANGUAGE_TYPESCRIPT,
	"javascript": wasm.Manifest_LANGUAGE_JAVASCRIPT,
	"luau":       wasm.Manifest_LANGUAGE_LUAU,
	"python":     wasm.Manifest_LANGUAGE_PYTHON,
	"go":         wasm.Manifest_LANGUAGE_GO,
}

// newCommand creates a mod project.
func newCommand(out *printer) *cli.Command {
	return &cli.Command{
		Name:      "new",
		Usage:     "create a mod project in a new directory named after the mod",
		ArgsUsage: "DIRECTORY",
		Flags: []cli.Flag{
			&cli.StringFlag{Name: "name", Usage: "the mod's display name (default: the directory name)"},
			&cli.StringFlag{Name: "language", Value: "typescript", Usage: "the source language: typescript, javascript, luau, python or go"},
		},
		Action: func(c *cli.Context) error {
			// Read the arguments.
			if c.NArg() != 1 {
				return errors.New("name the new project's directory, such as: modlock new my-mod")
			}
			language, ok := languages[c.String("language")]
			if !ok {
				return errors.Errorf("modlock cannot create %s mods yet", c.String("language"))
			}

			// Fetch this release's library, or the newest one from a
			// development build.
			version := Version
			if version == "dev" {
				version = "latest"
			}
			created, err := project.New(c.Context, c.Args().First(), c.String("name"), language, version, os.Stderr)
			if err != nil {
				return err
			}

			// Name the next step.
			out.note("created", created.Manifest.GetSlug(), "in", created.Dir)
			out.note("next: cd", c.Args().First(), "&& modlock dev")
			return nil
		},
	}
}
