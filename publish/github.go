package publish

import (
	"context"
	"io"
	"os/exec"

	"github.com/pkg/errors"
)

// GitHub attaches archive to the release tagged tag in the GitHub repository
// of the project in dir, creating the release when it does not exist. It
// runs the gh command line, which holds the creator's GitHub sign-in.
func GitHub(ctx context.Context, dir, tag, archive string, output io.Writer) error {
	gh := func(arguments ...string) *exec.Cmd {
		cmd := exec.CommandContext(ctx, "gh", arguments...)
		cmd.Dir = dir
		cmd.Stdout = output
		cmd.Stderr = output
		return cmd
	}
	if _, err := exec.LookPath("gh"); err != nil {
		return errors.New("publishing to GitHub needs the gh command line from https://cli.github.com")
	}
	view := gh("release", "view", tag)
	view.Stdout = io.Discard
	view.Stderr = io.Discard
	if view.Run() != nil {
		return errors.Wrap(gh("release", "create", tag, archive, "--title", tag, "--notes", "").Run(), "create the GitHub release")
	}
	return errors.Wrap(gh("release", "upload", tag, archive, "--clobber").Run(), "upload to the GitHub release")
}
