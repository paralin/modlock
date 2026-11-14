// Package selfupdate finds newer releases of the modlock command line and
// installs one over the running executable.
package selfupdate

import (
	"bytes"
	"context"
	"io/fs"
	"net/http"
	"os"
	"os/exec"
	"path"
	"path/filepath"
	"runtime"
	"strings"
	"time"

	"github.com/paralin/modlock/internal/fetch"
	"github.com/pkg/errors"
	"golang.org/x/mod/semver"
)

// latestURL redirects to the page of the newest release, whose path ends in
// its tag. Unlike the GitHub API, it has no rate limit.
const latestURL = "https://github.com/paralin/modlock/releases/latest"

// Released reports whether version names a release, such as v0.1.9, rather
// than a development build.
func Released(version string) bool {
	return semver.IsValid(version)
}

// Newer reports whether latest is a later release than current.
func Newer(latest, current string) bool {
	return Released(latest) && Released(current) && semver.Compare(latest, current) > 0
}

// Latest returns the tag of the newest release.
func Latest(ctx context.Context) (string, error) {
	// Ask for the newest release without following its redirect.
	request, err := http.NewRequestWithContext(ctx, http.MethodHead, latestURL, nil)
	if err != nil {
		return "", errors.Wrap(err, "find the latest release")
	}
	client := &http.Client{CheckRedirect: func(*http.Request, []*http.Request) error {
		return http.ErrUseLastResponse
	}}
	response, err := client.Do(request)
	if err != nil {
		return "", errors.Wrap(err, "find the latest release")
	}
	_ = response.Body.Close()

	// Read the tag from the address it redirects to.
	tag := path.Base(response.Header.Get("Location"))
	if !Released(tag) {
		return "", errors.Errorf("find the latest release: %s answered %s without a release tag", latestURL, response.Status)
	}
	return tag, nil
}

// Asset returns the name of the release file that holds the command line for
// this computer.
func Asset() string {
	name := "modlock-" + runtime.GOOS + "-" + runtime.GOARCH
	if runtime.GOOS == "windows" {
		name += ".exe"
	}
	return name
}

// Executable returns the file of the running command line, through any
// symbolic link that names it.
func Executable() (string, error) {
	exe, err := os.Executable()
	if err != nil {
		return "", errors.Wrap(err, "find the modlock executable")
	}
	resolved, err := filepath.EvalSymlinks(exe)
	if err != nil {
		return "", errors.Wrap(err, "find the modlock executable")
	}
	return resolved, nil
}

// Install replaces the executable at exe with the command line of version, a
// release or a build of master. It downloads the new file beside exe, runs it once to check
// that it starts and names version, and only then renames it into place, so
// a failed or interrupted update leaves the old command line working. An
// error wrapping fs.ErrPermission means exe's directory is not writable.
func Install(ctx context.Context, version, exe string) error {
	// Keep the old file's permissions.
	mode := fs.FileMode(0o755)
	if info, err := os.Stat(exe); err == nil {
		mode = info.Mode().Perm() | 0o100
	}

	// Download the release's file beside exe, which keeps the rename within
	// one filesystem. It keeps exe's extension, which Windows runs it by.
	ext := filepath.Ext(exe)
	staged := filepath.Join(filepath.Dir(exe), "."+strings.TrimSuffix(filepath.Base(exe), ext)+".update"+ext)
	defer os.Remove(staged)
	if err := fetch.ReleaseFile(ctx, version, Asset(), staged); err != nil {
		return err
	}
	if err := os.Chmod(staged, mode); err != nil {
		return errors.Wrap(err, "install the update")
	}

	// Check that the new file runs on this computer as the release asked for.
	checkCtx, cancel := context.WithTimeout(ctx, 30*time.Second)
	defer cancel()
	output, err := exec.CommandContext(checkCtx, staged, "--version").Output()
	if err != nil {
		return errors.Wrapf(err, "the downloaded %s does not run", Asset())
	}
	if !bytes.Contains(output, []byte(version)) && fetch.Master(version) {
		return errors.Errorf("master's build is being replaced right now; run modlock update again in a minute")
	}
	if !bytes.Contains(output, []byte(version)) {
		return errors.Errorf("the downloaded %s reports %q, not %s", Asset(), strings.TrimSpace(string(output)), version)
	}
	return swap(staged, exe)
}

// swap renames staged over exe. Windows cannot replace a running executable,
// but it can rename one, so there the old file moves aside first and Clean
// removes it on a later run.
func swap(staged, exe string) error {
	// Replace the file in one step where the system allows it.
	if runtime.GOOS != "windows" {
		return errors.Wrap(os.Rename(staged, exe), "install the update")
	}

	// Move the running file aside, and back again if the new one cannot take
	// its place.
	old := previous(exe)
	_ = os.Remove(old)
	if err := os.Rename(exe, old); err != nil {
		return errors.Wrap(err, "install the update")
	}
	if err := os.Rename(staged, exe); err != nil {
		_ = os.Rename(old, exe)
		return errors.Wrap(err, "install the update")
	}
	return nil
}

// Clean removes the file an update on Windows moved aside, once the command
// line that ran from it has exited. It does nothing elsewhere.
func Clean() {
	if runtime.GOOS != "windows" {
		return
	}
	if exe, err := Executable(); err == nil {
		_ = os.Remove(previous(exe))
	}
}

// previous names the file an update on Windows moves the old executable to.
func previous(exe string) string {
	return filepath.Join(filepath.Dir(exe), "."+filepath.Base(exe)+".old")
}
