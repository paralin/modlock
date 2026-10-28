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
		Description: "A script mod builds for QuickJS, which takes a moment. When its mod.json sets\n" +
			"\"runtime\": \"RUNTIME_WASM\", publish compiles it with scriptc instead, and otherwise\n" +
			"bundles and minifies it with Rolldown; --release builds that way here, to check the\n" +
			"published build.",
		Flags: []cli.Flag{releaseFlag},
		Action: func(c *cli.Context) error {
			// Open and build the project.
			opened, err := openProject(c)
			if err != nil {
				return err
			}
			if err := out.build(c.Context, opened, buildMode(c)); err != nil {
				return err
			}

			// Name the result.
			out.note("built", opened.Output())
			return nil
		},
	}
}

// releaseFlag asks build, dev and play for the build publish makes.
var releaseFlag = &cli.BoolFlag{
	Name:  "release",
	Usage: "build the mod as publish does: minified with Rolldown, or with scriptc when its mod.json asks for RUNTIME_WASM",
}

// buildMode is the build a command's --release flag asks for: a script mod
// bundles for QuickJS with esbuild, which builds in a moment, unless it asks
// for the build players run.
func buildMode(c *cli.Context) project.Mode {
	if c.Bool("release") {
		return project.Release
	}
	return project.Dev
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
