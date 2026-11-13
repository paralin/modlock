package main

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"

	"github.com/aperturerobotics/cli"
	"github.com/paralin/modlock/internal/prompt"
	"github.com/paralin/modlock/project"
	"github.com/paralin/modlock/src/wasm"
	"github.com/pkg/errors"
)

// language is one source language modlock new can write a project in.
type language struct {
	// flag is the language's --language value.
	flag string
	// label is the language as a person reads it.
	label string
	// manifest is the language in the mod's manifest.
	manifest wasm.Manifest_Language
}

// languages lists the languages in the order modlock new offers them, with
// the default first.
var languages = []language{
	{flag: "typescript", label: "TypeScript", manifest: wasm.Manifest_LANGUAGE_TYPESCRIPT},
	{flag: "javascript", label: "JavaScript", manifest: wasm.Manifest_LANGUAGE_JAVASCRIPT},
	{flag: "luau", label: "Luau", manifest: wasm.Manifest_LANGUAGE_LUAU},
	{flag: "python", label: "Python", manifest: wasm.Manifest_LANGUAGE_PYTHON},
	{flag: "go", label: "Go", manifest: wasm.Manifest_LANGUAGE_GO},
}

// newCommand creates a mod project. At a terminal it asks for whatever the
// arguments leave out; a script or --json gets no questions.
func newCommand(out *printer) *cli.Command {
	return &cli.Command{
		Name:      "new",
		Usage:     "create a mod project in a new directory, asking for its name and language",
		ArgsUsage: "[DIRECTORY]",
		Flags: []cli.Flag{
			&cli.StringFlag{Name: "name", Usage: "the mod's display name (default: the directory name in title case)"},
			&cli.StringFlag{Name: "language", Usage: "the source language: typescript, javascript, luau, python or go (default: asks, or typescript without a terminal)"},
		},
		Action: func(c *cli.Context) error {
			// Ask a person at a terminal for what the arguments leave out.
			if c.NArg() > 1 {
				return errors.New("modlock new takes one directory, such as: modlock new my-mod")
			}
			var ask *prompt.Terminal
			if !out.json && terminal(os.Stdin) && ansi(os.Stderr) {
				ask = prompt.NewTerminal(os.Stdin, os.Stderr, out.errColor)
			}
			dir, name, err := newTarget(ask, c.Args().First(), c.String("name"))
			if err != nil {
				return cancelled(err)
			}
			chosen, err := newLanguage(ask, c.String("language"))
			if err != nil {
				return cancelled(err)
			}

			// Fetch this release's library, or the newest one from a
			// development build.
			version := Version
			if version == "dev" {
				version = "latest"
			}
			created, err := project.New(c.Context, dir, name, chosen.manifest, version, os.Stderr)
			if err != nil {
				return err
			}
			out.note("created", created.Manifest.GetSlug(), "in", created.Dir)

			// Open the project in the editor, and give the next steps as
			// separate lines: Windows PowerShell has no && to join them.
			opened, err := openEditor(ask, created.Dir)
			if err != nil {
				return cancelled(err)
			}
			if !opened && chosen.manifest == wasm.Manifest_LANGUAGE_PYTHON {
				out.note("open the", dir, "folder itself in your editor, so that it finds the modlock library")
			}
			out.note("next: cd", dir)
			out.note("then: modlock dev")
			return nil
		},
	}
}

// newTarget returns the directory and display name of the project to create
// from the DIRECTORY argument and --name. With no directory it asks for the
// mod's name, which may be a display name such as "Zombie Tag" or a slug
// such as zombie-tag, and creates the project in the directory of its slug.
func newTarget(ask *prompt.Terminal, dir, name string) (string, string, error) {
	// Use the directory the arguments gave.
	if dir != "" {
		return dir, name, nil
	}
	if ask == nil {
		return "", "", errors.New("give the new project's directory, such as: modlock new my-mod")
	}

	// Ask for a name until its directory is free.
	answer, err := ask.Text("Mod name", "My Mod", func(answer string) error {
		slug := project.SlugFor(answer)
		if slug == "" {
			return errors.New("use at least one letter or digit")
		}
		return project.Vacant(slug)
	})
	if err != nil {
		return "", "", err
	}

	// Keep the answer as the display name unless it is already the slug.
	dir = project.SlugFor(answer)
	if name == "" && answer != dir {
		name = answer
	}
	return dir, name, nil
}

// newLanguage returns the language --language selects. With no flag it asks,
// or takes the first language when nobody is there to ask.
func newLanguage(ask *prompt.Terminal, flag string) (language, error) {
	// Take the flag's language.
	flag = strings.ToLower(strings.TrimSpace(flag))
	for _, l := range languages {
		if l.flag == flag {
			return l, nil
		}
	}
	if flag != "" {
		return language{}, errors.Errorf("modlock cannot create %s mods yet; choose typescript, javascript, luau, python or go", flag)
	}
	if ask == nil {
		return languages[0], nil
	}

	// Offer every language, and say which one needs something installed.
	options := make([]prompt.Option, len(languages))
	for i, l := range languages {
		options[i] = prompt.Option{Label: l.label}
		if _, err := exec.LookPath("go"); l.manifest == wasm.Manifest_LANGUAGE_GO && err != nil {
			options[i].Hint = "install Go first, from https://go.dev/dl/"
		}
	}
	selected, err := ask.Select("Language", options, 0)
	if err != nil {
		return language{}, err
	}
	return languages[selected], nil
}

// openEditor offers to open the new project's directory in Visual Studio
// Code, when its code command is installed, and reports whether it opened.
// An editor that opens the project's own folder finds the project's library
// and settings; one left on the folder above a Python project cannot resolve
// its import of modlock.
func openEditor(ask *prompt.Terminal, dir string) (bool, error) {
	// Offer it only to a person who has the editor.
	code, err := exec.LookPath("code")
	if ask == nil || err != nil {
		return false, nil
	}
	open, err := ask.Confirm("Open "+filepath.Base(dir)+" in VS Code?", true)
	if err != nil || !open {
		return false, err
	}

	// A failure to open leaves the project created, so carry on.
	return exec.Command(code, dir).Run() == nil, nil
}

// cancelled turns a question the person cancelled into a quiet exit with the
// status of an interrupt, and returns any other error unchanged.
func cancelled(err error) error {
	if errors.Is(err, prompt.ErrCancelled) {
		return cli.Exit("", 130)
	}
	return err
}
