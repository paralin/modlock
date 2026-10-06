package publish

import (
	"context"
	"io"
	"os/exec"

	"github.com/pkg/errors"
)

// GitHub attaches archive to the release tagged tag in the GitHub repository
// of the project in dir, creating the release with notes when it does not
// exist. It runs the gh command line, which holds the creator's GitHub
// sign-in.
func GitHub(ctx context.Context, dir, tag, archive, notes string, output io.Writer) error {
	// Run gh in the project, showing its output to the creator.
	gh := func(arguments ...string) *exec.Cmd {
		// Each command runs in dir and writes to output.
		cmd := exec.CommandContext(ctx, "gh", arguments...)
		cmd.Dir = dir
		cmd.Stdout = output
		cmd.Stderr = output
		return cmd
	}
	if _, err := exec.LookPath("gh"); err != nil {
		return errors.New("publishing to GitHub needs the gh command line from https://cli.github.com")
	}

	// Create the release when the tag has none, or replace its archive.
	view := gh("release", "view", tag)
	view.Stdout = io.Discard
	view.Stderr = io.Discard
	if view.Run() != nil {
		return errors.Wrap(gh("release", "create", tag, archive, "--title", tag, "--notes", notes).Run(), "create the GitHub release")
	}
	return errors.Wrap(gh("release", "upload", tag, archive, "--clobber").Run(), "upload to the GitHub release")
}
