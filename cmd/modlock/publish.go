package main

import (
	"os"
	"path/filepath"

	"github.com/aperturerobotics/cli"
	"github.com/paralin/modlock/check"
	"github.com/paralin/modlock/proto/modlock/publish"
	"github.com/pkg/errors"

	modpublish "github.com/paralin/modlock/publish"
)

// publishCommand builds, checks and publishes a mod.
func publishCommand(out *printer) *cli.Command {
	return &cli.Command{
		Name:      "publish",
		Usage:     "build and check the mod, then publish it",
		ArgsUsage: "[DIRECTORY]",
		Description: "hyperline uploads the mod to hyperline.gg, signing in through the browser the first time.\n" +
			"archive writes a zip players unpack and run with Play.cmd.\n" +
			"github attaches that zip to the project's GitHub release for the mod's version.",
		Flags: []cli.Flag{
			&cli.StringFlag{Name: "to", Value: "hyperline", Usage: "where to publish: hyperline, archive or github"},
			&cli.StringFlag{Name: "origin", Value: modpublish.DefaultOrigin, Usage: "the Hyperline service to publish to"},
			&cli.StringFlag{Name: "out", Value: ".", Usage: "the directory archive writes the zip to"},
		},
		Action: func(c *cli.Context) error {
			// Build and check the mod.
			opened, err := openProject(c)
			if err != nil {
				return err
			}
			if err := out.build(c.Context, opened); err != nil {
				return err
			}
			manifest, err := check.Mod(c.Context, os.DirFS(opened.Output()))
			if err != nil {
				return errors.Wrap(err, "check the built mod")
			}
			mod := &modpublish.Mod{Dir: opened.Output(), Manifest: manifest}
			name := manifest.GetSlug() + " " + manifest.GetVersion()

			// Publish to the chosen place.
			switch to := c.String("to"); to {
			case "hyperline":
				return publishHyperline(c, out, mod, name)
			case "archive":
				archive, err := modpublish.Archive(c.Context, mod, Version, c.String("out"))
				if err != nil {
					return err
				}
				out.published("archive", nil, archive)
				out.note("wrote", archive)
				return nil
			case "github":
				archive, err := modpublish.Archive(c.Context, mod, Version, os.TempDir())
				if err != nil {
					return err
				}
				defer os.Remove(archive)
				tag := manifest.GetSlug() + "-v" + manifest.GetVersion()
				if err := modpublish.GitHub(c.Context, opened.Dir, tag, archive, os.Stderr); err != nil {
					return err
				}
				out.published("github", nil, tag)
				out.note("published", name, "to the GitHub release", tag)
				return nil
			default:
				return errors.Errorf("--to names %q; choose hyperline, archive or github", to)
			}
		},
	}
}

// publishHyperline uploads mod to Hyperline and reports its review.
func publishHyperline(c *cli.Context, out *printer, mod *modpublish.Mod, name string) error {
	// Upload with the saved sign-in.
	config, err := os.UserConfigDir()
	if err != nil {
		return errors.Wrap(err, "find the configuration directory")
	}
	service := &modpublish.Hyperline{
		Origin:      c.String("origin"),
		SessionFile: filepath.Join(config, "modlock", "hyperline-session.json"),
		Output:      os.Stderr,
	}
	release, err := service.Publish(c.Context, mod)
	if err != nil {
		return err
	}

	// Report the review.
	out.published("hyperline", release, "")
	switch release.GetReviewState() {
	case publish.ReviewState_REVIEW_STATE_APPROVED:
		out.note("published " + name + "; players can install it now")
	case publish.ReviewState_REVIEW_STATE_REJECTED:
		return errors.Errorf("%s was rejected: %s", name, release.GetReviewNote())
	default:
		out.note("published " + name + "; it appears once a reviewer approves it")
	}
	return nil
}
