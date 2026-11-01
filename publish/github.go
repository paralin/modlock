package publish

import (
	"bytes"
	"context"
	"io"
	"net/url"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"time"

	"github.com/pkg/errors"
)

// GitHub attaches archive to the release tagged tag in the GitHub repository
// of the project in dir. A new release stays a draft until its asset uploads.
// Replacing a public asset keeps a local recovery copy until the replacement
// or restoration succeeds; a failed restoration reports the retained copy.
func GitHub(ctx context.Context, dir, tag, archive, notes string, output io.Writer) error {
	// Run gh with the creator's sign-in and show its progress.
	gh := func(callCtx context.Context, arguments ...string) *exec.Cmd {
		cmd := exec.CommandContext(callCtx, "gh", arguments...)
		cmd.Dir = dir
		cmd.Stdout, cmd.Stderr = output, output
		return cmd
	}
	if _, err := exec.LookPath("gh"); err != nil {
		return errors.New("publishing to GitHub needs the gh command line from https://cli.github.com")
	}

	// A new release is visible only after its upload finishes.
	var state bytes.Buffer
	view := gh(ctx, "release", "view", tag, "--json", "isDraft", "--jq", ".isDraft")
	view.Stdout, view.Stderr = &state, io.Discard
	if err := view.Run(); err != nil {
		if err := gh(ctx, "release", "create", tag, archive, "--draft", "--title", tag, "--notes", notes).Run(); err != nil {
			return errors.Wrap(err, "create the GitHub draft; retry publishing to finish it")
		}
	} else if strings.TrimSpace(state.String()) == "false" {
		return replaceGitHubAsset(ctx, tag, archive, gh)
	} else if err := gh(ctx, "release", "upload", tag, archive, "--clobber").Run(); err != nil {
		return errors.Wrap(err, "upload to the GitHub draft; retry publishing to finish it")
	}

	// An existing draft and a newly completed draft share the final step.
	return errors.Wrap(gh(ctx, "release", "edit", tag, "--draft=false").Run(), "publish the completed GitHub draft")
}

// replaceGitHubAsset replaces a public asset, restoring its previous bytes
// after an upload failure. A failed restoration retains the recovery copy.
func replaceGitHubAsset(ctx context.Context, tag, archive string, gh func(context.Context, ...string) *exec.Cmd) error {
	// Resolve this exact asset, without interpreting its name as a glob.
	var asset bytes.Buffer
	endpoint := "repos/{owner}/{repo}/releases/tags/" + url.PathEscape(tag)
	query := gh(ctx, "api", endpoint, "--jq", ".assets[] | select(.name == env.MODLOCK_RELEASE_ASSET) | .id")
	query.Env = append(os.Environ(), "MODLOCK_RELEASE_ASSET="+filepath.Base(archive))
	query.Stdout = &asset

	// An unknown asset identity leaves the published release untouched.
	if err := query.Run(); err != nil {
		return errors.Wrap(err, "find the previous GitHub asset")
	}
	id := strings.TrimSpace(asset.String())
	if id == "" {
		return errors.Wrap(gh(ctx, "release", "upload", tag, archive, "--clobber").Run(), "upload to the GitHub release")
	}
	if _, err := strconv.ParseUint(id, 10, 64); err != nil {
		return errors.Wrap(err, "read the previous GitHub asset identity")
	}

	// Download the recovery copy before clobber can delete the public asset.
	recoveryDir, err := os.MkdirTemp("", "modlock-release-")
	if err != nil {
		return err
	}
	keep := false
	defer func() {
		if !keep {
			_ = os.RemoveAll(recoveryDir)
		}
	}()

	// Capture the previous asset without buffering the archive in memory.
	backup := filepath.Join(recoveryDir, filepath.Base(archive))
	file, err := os.OpenFile(backup, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0o600)
	if err != nil {
		return err
	}
	download := gh(ctx, "api", "repos/{owner}/{repo}/releases/assets/"+id, "-H", "Accept: application/octet-stream")
	download.Stdout = file
	downloadErr := download.Run()

	// Complete the local copy before allowing clobber to delete its source.
	closeErr := file.Close()
	if downloadErr != nil {
		return errors.Wrap(downloadErr, "save the previous GitHub asset before replacing it")
	}
	if closeErr != nil {
		return errors.Wrap(closeErr, "close the previous GitHub asset copy")
	}

	// A cancelled publish still gets a bounded attempt to restore the old asset.
	if err := gh(ctx, "release", "upload", tag, archive, "--clobber").Run(); err != nil {
		recovery, cancel := context.WithTimeout(context.WithoutCancel(ctx), 10*time.Minute)
		defer cancel()
		if restoreErr := gh(recovery, "release", "upload", tag, backup, "--clobber").Run(); restoreErr != nil {
			keep = true
			return errors.Wrapf(err, "replace GitHub asset; restoring it also failed (%v); previous asset kept at %s", restoreErr, backup)
		}
		return errors.Wrap(err, "replace GitHub asset; its previous version was restored")
	}
	return nil
}
