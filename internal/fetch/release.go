package fetch

import (
	"context"
	"io"
	"net/http"
	"regexp"
	"strings"

	"github.com/pkg/errors"
	"golang.org/x/mod/semver"
)

// MasterTag is the prerelease that holds the newest build of the master
// branch. Each push to master replaces its files.
const MasterTag = "master-build"

// releases is the address the files of every release start with.
const releases = "https://github.com/paralin/modlock/releases/download/"

// masterVersion is the file in the master prerelease that names the build its
// other files belong to.
const masterVersion = "version"

// masterBuild matches the version of a build of the master branch: the first
// twelve digits of its commit.
var masterBuild = regexp.MustCompile(`^master-[0-9a-f]{12}$`)

// ErrSuperseded reports that the master prerelease no longer holds the build
// asked for, because a later push replaced it.
var ErrSuperseded = errors.New("master has moved on from this build; run modlock update")

// MasterVersion returns the version of the build of master at commit.
func MasterVersion(commit string) string {
	return "master-" + commit[:min(len(commit), 12)]
}

// Master reports whether version names a build of the master branch, such as
// master-0123456789ab.
func Master(version string) bool {
	return masterBuild.MatchString(version)
}

// Published reports whether version has files to download: a release, such
// as v0.1.9, or a build of master, but not a development build.
func Published(version string) bool {
	return semver.IsValid(version) || Master(version)
}

// Release returns the address of asset among the files of version: its own
// release, or the master prerelease for a build of master.
func Release(version, asset string) string {
	tag := version
	if Master(version) {
		tag = MasterTag
	}
	return releases + tag + "/" + asset
}

// LatestMaster returns the version of the build the master prerelease holds.
func LatestMaster(ctx context.Context) (string, error) {
	// Read the file that names the build.
	url := releases + MasterTag + "/" + masterVersion
	request, err := http.NewRequestWithContext(ctx, http.MethodGet, url, nil)
	if err != nil {
		return "", errors.Wrap(err, "find the newest build of master")
	}
	response, err := http.DefaultClient.Do(request)
	if err != nil {
		return "", errors.Wrap(err, "find the newest build of master")
	}
	defer response.Body.Close()

	// It is a few bytes long.
	if response.StatusCode != http.StatusOK {
		return "", errors.Errorf("find the newest build of master: %s answered %s", url, response.Status)
	}
	data, err := io.ReadAll(io.LimitReader(response.Body, 64))
	if err != nil {
		return "", errors.Wrap(err, "find the newest build of master")
	}

	// It holds one version.
	version := strings.TrimSpace(string(data))
	if !Master(version) {
		return "", errors.Errorf("find the newest build of master: %s holds %q, not a build", url, version)
	}
	return version, nil
}

// ReleaseFile writes asset from the files of version to path. A build of
// master shares one prerelease with every later build, so ReleaseFile checks
// after the download that the prerelease still holds version, and returns
// ErrSuperseded when it does not: the file may belong to a later build.
func ReleaseFile(ctx context.Context, version, asset, path string) error {
	// Download the file; a release's is its own for good.
	if err := File(ctx, Release(version, asset), path); err != nil {
		return err
	}
	if !Master(version) {
		return nil
	}

	// Check that master's build did not change under the download.
	latest, err := LatestMaster(ctx)
	if err != nil {
		return err
	}
	if latest != version {
		return ErrSuperseded
	}
	return nil
}
