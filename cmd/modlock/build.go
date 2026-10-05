package main

import (
	"github.com/aperturerobotics/cli"
	"github.com/paralin/modlock/project"
)

// buildCommand checks and builds a mod project.
func buildCommand(out *printer) *cli.Command {
	return &cli.Command{
		Name:      "build",
		Usage:     "check the mod and write the built mod to its build directory",
		ArgsUsage: "[DIRECTORY]",
		Action: func(c *cli.Context) error {
			// Open and build the project.
			opened, err := openProject(c)
			if err != nil {
				return err
			}
			if err := out.build(c.Context, opened); err != nil {
				return err
			}

			// Name the result.
			out.note("built", opened.Output())
			return nil
		},
	}
}

// openProject opens the project named by the first argument, or the current
// directory.
func openProject(c *cli.Context) (*project.Project, error) {
	dir := c.Args().First()
	if dir == "" {
		dir = "."
	}
	return project.Open(dir)
}
