// Package npm downloads pinned npm packages into the user cache: the
// TypeScript checker, the runtime the TypeScript library imports, and the
// bundler that builds a release.
package npm

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"maps"
	"os"
	"path"
	"path/filepath"
	"runtime"
	"slices"

	"github.com/paralin/modlock/internal/fetch"
	"github.com/pkg/errors"
)

// TsgoVersion is the TypeScript native preview that checks script mods and
// declares the library.
const TsgoVersion = "7.0.0-dev.20260707.2"

// Package returns the directory holding the npm package name at version,
// downloading it from the registry into the user cache the first time.
func Package(ctx context.Context, name, version string) (string, error) {
	// Download and unpack the package the first time it is asked for.
	dir, err := fetch.Cache("npm", name+"@"+version)
	if err != nil {
		return "", err
	}
	err = fetch.Install(dir, func(staging string) error {
		return unpack(ctx, name, version, dir+".tgz", staging)
	})
	if err != nil {
		return "", errors.Wrapf(err, "install %s", name)
	}
	return dir, nil
}

// Modules returns a directory whose node_modules holds each of packages, a
// name to version map, so the packages resolve one another as an npm install
// lays them out. It installs no dependencies packages leaves out.
func Modules(ctx context.Context, packages map[string]string) (string, error) {
	// Key the tree by its packages.
	names := slices.Sorted(maps.Keys(packages))
	key := sha256.New()
	for _, name := range names {
		_, _ = key.Write([]byte(name + "@" + packages[name] + "\x00"))
	}
	dir, err := fetch.Cache("npm", "modules", hex.EncodeToString(key.Sum(nil))[:16])
	if err != nil {
		return "", err
	}

	// Unpack each package into the tree the first time it is asked for.
	err = fetch.Install(dir, func(staging string) error {
		for _, name := range names {
			target := filepath.Join(staging, "node_modules", filepath.FromSlash(name))
			if err := unpack(ctx, name, packages[name], staging+".tgz", target); err != nil {
				return errors.Wrapf(err, "install %s", name)
			}
		}
		return nil
	})
	if err != nil {
		return "", err
	}
	return dir, nil
}

// unpack downloads the registry's archive of name at version to archive, then
// unpacks the package into dir.
func unpack(ctx context.Context, name, version, archive, dir string) error {
	defer os.Remove(archive)
	url := "https://registry.npmjs.org/" + name + "/-/" + path.Base(name) + "-" + version + ".tgz"
	if err := fetch.File(ctx, url, archive); err != nil {
		return err
	}
	return fetch.UntarTree(archive, "package/", dir)
}

// Tsgo returns the TypeScript checker for this system.
func Tsgo(ctx context.Context) (string, error) {
	dir, err := Package(ctx, "@typescript/native-preview-"+host(), TsgoVersion)
	if err != nil {
		return "", err
	}
	return filepath.Join(dir, "lib", executable("tsgo")), nil
}

// host names this system's platform and architecture as npm does.
func host() string {
	// Map Go's names to npm's where they differ.
	system := map[string]string{"windows": "win32"}[runtime.GOOS]
	if system == "" {
		system = runtime.GOOS
	}
	arch := map[string]string{"amd64": "x64"}[runtime.GOARCH]
	if arch == "" {
		arch = runtime.GOARCH
	}
	return system + "-" + arch
}

// executable returns the file name of the command name on this system.
func executable(name string) string {
	if runtime.GOOS == "windows" {
		return name + ".exe"
	}
	return name
}
